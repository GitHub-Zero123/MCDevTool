#pragma once

#include <atomic>
#include <cstddef>
#include <deque>
#include <expected>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <mcp_tool.h>

namespace mcdk::runtime {

    enum class McpToolBindError {
        InvalidName,
        DuplicateName,
        EmptyHandler,
        RegistrySealed,
        // attach 专用：清单里没有这个名字，或它不属于调用方，或已经绑过了。
        NotDeclared,
        OwnerMismatch,
        AlreadyBound,
    };

    [[nodiscard]] std::string_view describeMcpToolBindError(McpToolBindError error) noexcept;

    // 与 mcp::tool_handler 逐字相同（mcp::json 是 ordered_json，不是 nlohmann::json）。
    // 这里仅声明所需类型，避免公开头文件依赖完整的 mcp_server.h。
    using McpToolHandler = std::function<mcp::json(const mcp::json& params, const std::string& sessionId)>;

    struct McpToolEntry {
        mcp::tool      descriptor;
        McpToolHandler handler;
        // 注册来源，"builtin" 或插件 id。重名时用它给出可定位的报错。
        std::string owner;
        // declare 建的条目在 attach 之前为 false，此时 handler 是个只会报错的占位。
        bool bound = true;
    };

    // MCP 工具注册表。形态对齐 RpcRegistry：注册窗口关闭后封存，运行期只读。
    // 它把"有哪些工具"与"MCP 服务器"解耦：内置工具与插件工具注册进同一张表，
    class McpToolRegistry {
    public:
        McpToolRegistry()                                  = default;
        McpToolRegistry(const McpToolRegistry&)            = delete;
        McpToolRegistry& operator=(const McpToolRegistry&) = delete;

        // 重名一律失败，禁止后注册者覆盖先注册者——否则插件加载顺序会悄悄改变
        // AI 看到的工具语义，这类问题排查时几乎无迹可循。
        [[nodiscard]] std::expected<void, McpToolBindError>
        bind(mcp::tool descriptor, McpToolHandler handler, std::string owner);

        // 清单声明的工具。描述符此刻就定下来，handler 等插件在 REGISTER 阶段 attach。
        [[nodiscard]] std::expected<void, McpToolBindError> declare(mcp::tool descriptor, std::string owner);

        // 给已声明的工具装上实现。owner 必须与声明者一致，否则插件之间可以互相顶替。
        [[nodiscard]] std::expected<void, McpToolBindError>
        attach(std::string_view name, McpToolHandler handler, std::string_view owner);

        // 声明了却没人 attach 的工具名。封存前用它拦住「清单列了但调不通」。
        [[nodiscard]] std::vector<std::string> unboundTools() const;

        void               seal();
        [[nodiscard]] bool sealed() const noexcept;

        [[nodiscard]] const McpToolEntry* find(std::string_view name) const;
        [[nodiscard]] std::size_t         size() const;

        // 按注册顺序遍历，保证发布过程与诊断输出可复现。
        // 注意客户端实际看到的工具顺序由 mcp::server 内部的 std::map 按名字排序决定，与此无关。
        void forEach(const std::function<void(const McpToolEntry&)>& visitor) const;

    private:
        struct TransparentStringHash {
            using is_transparent = void;

            [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
                return std::hash<std::string_view>{}(value);
            }

            [[nodiscard]] std::size_t operator()(const std::string& value) const noexcept {
                return operator()(std::string_view(value));
            }
        };

        // deque 而非 vector：find 返回条目指针，追加时不能让已发出的指针失效。
        std::deque<McpToolEntry>                                                             mEntries;
        std::unordered_map<std::string, std::size_t, TransparentStringHash, std::equal_to<>> mIndex;

        mutable std::mutex mMutex;
        std::atomic<bool>  mSealed = false;
    };

} // namespace mcdk::runtime
