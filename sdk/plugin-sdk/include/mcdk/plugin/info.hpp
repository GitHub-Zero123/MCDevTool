#pragma once
// mcdk.info 的 C++ 封装。
// ABI 上 mcdk_session_info 的字符串字段全是借用的，get_session 一返回就失效。
#include <cstdint>
#include <string>

#include "abi/iface/info.h"
#include "detail/abi_bridge.hpp"
#include "error.hpp"

namespace mcdk {

    // 一次会话的快照。字段语义见 docs/plugin-system/05-interfaces.md §5。
    struct SessionInfo {
        std::uint32_t mcdkPid = 0;
        // 0 表示游戏进程尚未创建。
        std::uint32_t gamePid = 0;
        // 0 表示调试 IPC 未启用。
        std::uint16_t gameIpcPort = 0;
        // 0 表示 MCP 未启用。
        std::uint16_t mcpPort    = 0;
        bool          mcpEnabled = false;
        // 调试 IPC 已有客户端，即游戏已进入世界。
        bool gameDebugReady = false;

        std::string mcpIp;
        std::string gameExePath;
        std::string projectRoot;
        std::string worldName;
        std::string worldFolderName;
        std::string worldRuntimePath;
        // 空表示非玩法地图工程。
        std::string worldSourcePath;
    };

    class Info {
    public:
        Info() = default;

        Info(mcdk_handle self, const mcdk_iface_info* table) noexcept : mSelf(self), mTable(table) {}

        [[nodiscard]] bool available() const noexcept { return mTable != nullptr; }
// 取一份会话快照。
// gamePid 与 gameDebugReady 会随时间变化，不要缓存后长期使用；需要跟踪
        [[nodiscard]] SessionInfo session() const {
            SessionInfo result;
            if (!detail::ifaceHas(mTable, &mcdk_iface_info::get_session)) {
                return result;
            }
            mcdk_session_info raw{};
            raw.struct_size = static_cast<std::uint32_t>(sizeof(raw));
            if (mTable->get_session(mSelf, &raw) != MCDK_OK) {
                return result;
            }

            result.mcdkPid        = raw.mcdk_pid;
            result.gamePid        = raw.game_pid;
            result.gameIpcPort    = raw.game_ipc_port;
            result.mcpPort        = raw.mcp_port;
            result.mcpEnabled     = raw.mcp_enabled != MCDK_FALSE;
            result.gameDebugReady = raw.game_debug_ready != MCDK_FALSE;

            // 必须在这里立刻拷贝：raw 里的指针指向宿主的线程局部缓冲。
            result.mcpIp            = detail::toString(raw.mcp_ip);
            result.gameExePath      = detail::toString(raw.game_exe_path);
            result.projectRoot      = detail::toString(raw.project_root);
            result.worldName        = detail::toString(raw.world_name);
            result.worldFolderName  = detail::toString(raw.world_folder_name);
            result.worldRuntimePath = detail::toString(raw.world_runtime_path);
            result.worldSourcePath  = detail::toString(raw.world_source_path);
            return result;
        }

    private:
        mcdk_handle            mSelf  = 0;
        const mcdk_iface_info* mTable = nullptr;
    };

} // namespace mcdk
