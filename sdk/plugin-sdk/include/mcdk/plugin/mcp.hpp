#pragma once
// mcdk.mcp 的 C++ 封装。
// MCP 接口在 ABI 层以 JSON 文本传递复杂参数。
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "abi/iface/mcp.h"
#include "detail/abi_bridge.hpp"
#include "detail/barrier.hpp"
#include "error.hpp"

namespace mcdk {

    // MCP 工具的注解。std::optional 区分「未设置」和显式 false。
    // 过界时会被拆成 present / value 两个位掩码，正是为了保住这个区别。
    struct ToolAnnotations {
        std::optional<std::string> title;
        std::optional<bool>        readOnly;
        std::optional<bool>        destructive;
        std::optional<bool>        idempotent;
        std::optional<bool>        openWorld;
    };

    struct ToolDesc {
        std::string     name;
        std::string     description;
        // JSON Schema 对象的文本形式，必填。
        std::string     inputSchema;
        // 可选。空表示未设置。
        std::string     outputSchema;
        ToolAnnotations annotations;
    };

    class Mcp {
    public:
        Mcp() = default;

        Mcp(mcdk_handle self, const mcdk_iface_mcp* table) noexcept : mSelf(self), mTable(table) {}

        [[nodiscard]] bool available() const noexcept { return mTable != nullptr; }
        // 给 plugin.json 的 mcpTools 里声明过的工具装上实现。描述符写在清单里，
        // 代码里只出现名字——所以 mcdk 没跑的时候 stdio bridge 也能列出这个工具。
        // 声明了不绑、或绑了没声明，都会让插件加载失败。
        // 线程约束与 addTool 相同。
        template <class Handler>
        mcdk_status bindTool(std::string_view name, Handler&& handler) {
            if (!detail::ifaceHas(mTable, &mcdk_iface_mcp::bind_tool)) {
                return MCDK_ERR_NOT_SUPPORTED;
            }

            using Stored = std::decay_t<Handler>;
            auto  holder = std::make_unique<Holder<Stored>>(std::forward<Handler>(handler));
            void* user   = holder.get();

            const auto status = mTable->bind_tool(mSelf, detail::toAbi(name), &trampoline<Stored>, user);
            if (status != MCDK_OK) {
                return status;
            }
            mHolders.push_back(std::move(holder));
            return MCDK_OK;
        }

        // 运行期注册一个 MCP 工具，描述符随调用传过去。
        // 与 bindTool 的取舍：动态注册的工具只在本进程活着时存在，mcdk 没跑的时候
        // stdio bridge 列不出来，AI 也就发现不了。能写进清单的都应该用 bindTool。
        // 只能在 REGISTER 阶段调用，通常写在 ev::McpRegisterBefore 处理器中。
        // handler 跑在 **MCP 工作线程**上，且**可能被并发调用**（默认上限 8），
        // 必须自行保证线程安全。允许阻塞——这正是它调 game().executePython 的场景。
        template <class Handler>
        mcdk_status addTool(const ToolDesc& desc, Handler&& handler) {
            if (!detail::ifaceHas(mTable, &mcdk_iface_mcp::add_tool)) {
                return MCDK_ERR_NOT_SUPPORTED;
            }

            using Stored = std::decay_t<Handler>;
            auto  holder = std::make_unique<Holder<Stored>>(std::forward<Handler>(handler));
            void* user   = holder.get();

            mcdk_mcp_tool_desc raw{};
            raw.struct_size = static_cast<std::uint32_t>(sizeof(raw));
            raw.name        = detail::toAbi(desc.name);
            raw.description = detail::toAbi(desc.description);
            raw.input_schema_json  = detail::toAbi(desc.inputSchema);
            raw.output_schema_json = detail::toAbi(desc.outputSchema);
            if (desc.annotations.title) {
                raw.title = detail::toAbi(*desc.annotations.title);
            }
            pack(raw, desc.annotations.readOnly, MCDK_MCP_ANNOTATION_READ_ONLY);
            pack(raw, desc.annotations.destructive, MCDK_MCP_ANNOTATION_DESTRUCTIVE);
            pack(raw, desc.annotations.idempotent, MCDK_MCP_ANNOTATION_IDEMPOTENT);
            pack(raw, desc.annotations.openWorld, MCDK_MCP_ANNOTATION_OPEN_WORLD);

            const auto status = mTable->add_tool(mSelf, &raw, &trampoline<Stored>, user);
            if (status != MCDK_OK) {
                return status;
            }
            // 闭包必须活过注册；由本对象持有，随插件实例一起销毁。
            mHolders.push_back(std::move(holder));
            return MCDK_OK;
        }

        // 当前已注册的全部工具，JSON 数组文本。
        [[nodiscard]] std::string listTools() const {
            if (!detail::ifaceHas(mTable, &mcdk_iface_mcp::list_tools)) {
                return "[]";
            }
            mcdk_str raw{};
            if (mTable->list_tools(mSelf, &raw) != MCDK_OK) {
                return "[]";
            }
            return detail::toString(raw);
        }

    private:
        struct HolderBase {
            virtual ~HolderBase()                                                         = default;
            virtual std::string invoke(std::string_view arguments, std::string_view session) = 0;
        };

        template <class Fn>
        struct Holder final : HolderBase {
            explicit Holder(Fn&& value) : fn(std::move(value)) {}
            explicit Holder(const Fn& value) : fn(value) {}

            std::string invoke(std::string_view arguments, std::string_view session) override {
                return fn(arguments, session);
            }

            Fn fn;
        };

        // 结果与错误文本共用同一个 TLS 缓冲：宿主在 handler 返回后立即拷走，
        // 同线程的下一次调用才会覆盖它（05-interfaces.md §8.4）。
        [[nodiscard]] static std::string& resultSlot() noexcept {
            thread_local std::string storage;
            return storage;
        }

        template <class Fn>
        static mcdk_status MCDK_CALL trampoline(
            void*     user,
            mcdk_str  arguments_json,
            mcdk_str  session_id,
            mcdk_str* out_result_json
        ) {
            if (out_result_json == nullptr) {
                return MCDK_ERR_INVALID_ARGUMENT;
            }
            *out_result_json = mcdk_str{};
            if (user == nullptr) {
                return MCDK_ERR_INVALID_HANDLE;
            }
            const auto status = detail::guard(
                [&]() -> mcdk_status {
                    resultSlot() = static_cast<Holder<Fn>*>(user)->invoke(
                        detail::toView(arguments_json),
                        detail::toView(session_id)
                    );
                    return MCDK_OK;
                },
                MCDK_ERR_PLUGIN_EXCEPTION
            );
            if (status != MCDK_OK) {
                // 失败时把屏障记下的消息放进同一个槽——宿主拿它转成 MCP 错误响应。
                resultSlot() = detail::errorSlot().message;
            }
            out_result_json->ptr = resultSlot().data();
            out_result_json->len = resultSlot().size();
            return status;
        }

        static void pack(mcdk_mcp_tool_desc& raw, const std::optional<bool>& hint, std::uint32_t bit) {
            if (!hint.has_value()) {
                return;
            }
            raw.annotation_present |= bit;
            if (*hint) {
                raw.annotation_value |= bit;
            }
        }

        mcdk_handle                              mSelf  = 0;
        const mcdk_iface_mcp*                    mTable = nullptr;
        std::vector<std::unique_ptr<HolderBase>> mHolders;
    };

} // namespace mcdk
