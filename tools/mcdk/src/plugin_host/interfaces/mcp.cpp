//
// mcdk.mcp/1 的宿主实现。
//
// 本文件里的每个导出函数都必须经过 guard / guardVoid，没有例外
// （docs/plugin-system/02-abi-contract.md §4.3）。
//
// 这是六张表里唯一一张要把 C++ 复杂类型（mcp::tool 的两棵 JSON 树 + 五个
// std::optional）在边界两侧各自消化掉的。降级规则见 05-interfaces.md §8.1。
//

#include <optional>
#include <string>
#include <string_view>

#include <mcp_tool.h>

#include <mcdk/plugin/abi/iface/mcp.h>
#include <mcdk/plugin_host/guard.hpp>
#include <mcdk/runtime/mcp_tool_registry.hpp>

#include "../registry.hpp"

namespace mcdk::plugin_host::detail {

    namespace {

        [[nodiscard]] std::string_view toView(mcdk_str text) noexcept {
            if (text.ptr == nullptr || text.len == 0) {
                return {};
            }
            return std::string_view(text.ptr, text.len);
        }

        // list_tools 的结果按借用交付，与 mcdk.game 的 execute_python 同一套约定。
        [[nodiscard]] std::string& listSlot() noexcept {
            thread_local std::string storage;
            return storage;
        }

        // 只在 present 位被置位时才写入 optional —— 这正是 present/value 两个掩码
        // 存在的理由：单独一个 value 掩码无法区分「显式设为 false」与「没设」。
        void applyHint(std::optional<bool>& target, std::uint32_t present, std::uint32_t value, std::uint32_t bit) {
            if ((present & bit) != 0) {
                target = (value & bit) != 0;
            }
        }

        [[nodiscard]] bool parseSchema(std::string_view text, mcp::json& out, std::string& error) {
            try {
                out = mcp::json::parse(text);
            } catch (const std::exception& ex) {
                error = ex.what();
                return false;
            }
            if (!out.is_object()) {
                error = "schema 必须是 JSON 对象";
                return false;
            }
            return true;
        }

        mcdk_status MCDK_CALL mcpAddTool(
            mcdk_handle               self,
            const mcdk_mcp_tool_desc* desc,
            mcdk_mcp_tool_handler     handler,
            void*                     user
        ) noexcept {
            return guard([&]() -> mcdk_status {
                if (desc == nullptr || handler == nullptr) {
                    return MCDK_ERR_INVALID_ARGUMENT;
                }
                if (desc->struct_size < sizeof(mcdk_mcp_tool_desc)) {
                    return MCDK_ERR_INVALID_ARGUMENT;
                }
                const auto* record = registry().find(self);
                if (record == nullptr) {
                    return MCDK_ERR_INVALID_HANDLE;
                }
                if (currentStage() != MCDK_STAGE_REGISTER) {
                    return MCDK_ERR_WRONG_STAGE;
                }

                const auto binding = sessionBinding();
                if (!binding->mcpToolRegistry) {
                    return MCDK_ERR_NOT_SUPPORTED;
                }

                const auto name = toView(desc->name);
                if (name.empty()) {
                    return setError(MCDK_ERR_INVALID_ARGUMENT, "工具名不能为空");
                }

                mcp::tool tool;
                tool.name        = std::string(name);
                tool.description = std::string(toView(desc->description));

                std::string parseError;
                if (!parseSchema(toView(desc->input_schema_json), tool.parameters_schema, parseError)) {
                    return setError(
                        MCDK_ERR_INVALID_ARGUMENT,
                        "工具 " + tool.name + " 的 input_schema 解析失败：" + parseError
                    );
                }
                if (desc->output_schema_json.len != 0
                    && !parseSchema(toView(desc->output_schema_json), tool.output_schema, parseError)) {
                    return setError(
                        MCDK_ERR_INVALID_ARGUMENT,
                        "工具 " + tool.name + " 的 output_schema 解析失败：" + parseError
                    );
                }

                if (desc->title.len != 0) {
                    tool.annotations.title = std::string(toView(desc->title));
                }
                applyHint(
                    tool.annotations.read_only_hint,
                    desc->annotation_present,
                    desc->annotation_value,
                    MCDK_MCP_ANNOTATION_READ_ONLY
                );
                applyHint(
                    tool.annotations.destructive_hint,
                    desc->annotation_present,
                    desc->annotation_value,
                    MCDK_MCP_ANNOTATION_DESTRUCTIVE
                );
                applyHint(
                    tool.annotations.idempotent_hint,
                    desc->annotation_present,
                    desc->annotation_value,
                    MCDK_MCP_ANNOTATION_IDEMPOTENT
                );
                applyHint(
                    tool.annotations.open_world_hint,
                    desc->annotation_present,
                    desc->annotation_value,
                    MCDK_MCP_ANNOTATION_OPEN_WORLD
                );

                // handler 跨界的形态是「函数指针 + void* user」，这里把它包成
                // std::function。捕获 self 而非 record 指针：句柄作废后要能查得出来。
                const std::string owner   = record->id;
                const std::string toolName = tool.name;
                auto              bound   = [self, handler, user, owner, toolName](
                                   const mcp::json&   params,
                                   const std::string& sessionId
                               ) -> mcp::json {
                    if (registry().find(self) == nullptr) {
                        // 插件已被终结，但 MCP 服务器还挂着它的工具。
                        return mcp::json{
                            {"isError", true},
                            {"content",
                             mcp::json::array(
                                 {mcp::json{{"type", "text"}, {"text", "插件 " + owner + " 已卸载，工具不再可用"}}}
                             )},
                        };
                    }
                    const std::string argumentsText = params.dump();
                    mcdk_str          rawResult{};
                    const mcdk_status status = handler(
                        user,
                        mcdk_str{argumentsText.data(), argumentsText.size()},
                        mcdk_str{sessionId.data(), sessionId.size()},
                        &rawResult
                    );
                    if (status != MCDK_OK) {
                        // 失败时从插件错误槽取消息。取不到就只报状态码，
                        // 总比把一个空字符串当成结果发给 AI 强。
                        std::string message = std::string(toView(rawResult));
                        if (message.empty()) {
                            message = "工具 " + toolName + " 失败，status=" + std::to_string(status);
                        }
                        return mcp::json{
                            {"isError", true},
                            {"content", mcp::json::array({mcp::json{{"type", "text"}, {"text", message}}})},
                        };
                    }
                    // 借用的，立即拷贝再解析。
                    const std::string resultText(toView(rawResult));
                    try {
                        return mcp::json::parse(resultText);
                    } catch (const std::exception&) {
                        // 插件返回了非 JSON，按纯文本结果处理而不是把整个调用打成失败。
                        return mcp::json{
                            {"content", mcp::json::array({mcp::json{{"type", "text"}, {"text", resultText}}})},
                        };
                    }
                };

                const auto bindResult = binding->mcpToolRegistry->bind(std::move(tool), std::move(bound), owner);
                if (bindResult) {
                    return MCDK_OK;
                }
                const mcdk_status code = bindResult.error() == runtime::McpToolBindError::DuplicateName
                                           ? MCDK_ERR_DUPLICATE
                                       : bindResult.error() == runtime::McpToolBindError::RegistrySealed
                                           ? MCDK_ERR_WRONG_STAGE
                                           : MCDK_ERR_INVALID_ARGUMENT;
                return setError(
                    code,
                    "注册工具 " + toolName + " 失败："
                        + std::string(runtime::describeMcpToolBindError(bindResult.error()))
                );
            });
        }

        mcdk_status MCDK_CALL mcpListTools(mcdk_handle self, mcdk_str* out_json) noexcept {
            return guard([&]() -> mcdk_status {
                if (out_json == nullptr) {
                    return MCDK_ERR_INVALID_ARGUMENT;
                }
                *out_json = mcdk_str{};
                if (registry().find(self) == nullptr) {
                    return MCDK_ERR_INVALID_HANDLE;
                }
                const auto binding = sessionBinding();
                if (!binding->mcpToolRegistry) {
                    return MCDK_ERR_NOT_SUPPORTED;
                }
                auto array = mcp::json::array();
                binding->mcpToolRegistry->forEach([&array](const runtime::McpToolEntry& entry) {
                    auto item     = entry.descriptor.to_json();
                    item["owner"] = entry.owner;
                    array.push_back(std::move(item));
                });
                listSlot()    = array.dump();
                out_json->ptr = listSlot().data();
                out_json->len = listSlot().size();
                return MCDK_OK;
            });
        }

        constexpr mcdk_iface_mcp kTable = {
            /* struct_size */ static_cast<uint32_t>(sizeof(mcdk_iface_mcp)),
            /* _reserved   */ 0u,
            /* add_tool    */ &mcpAddTool,
            /* list_tools  */ &mcpListTools,
        };

    } // namespace

    const mcdk_iface_mcp* mcpTable() noexcept { return &kTable; }

} // namespace mcdk::plugin_host::detail
