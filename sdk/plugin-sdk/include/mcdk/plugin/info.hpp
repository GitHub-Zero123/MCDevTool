#pragma once
// mcdk.info 的 C++ 封装。
// ABI 上 mcdk_session_info 的字符串字段全是借用的，get_session 一返回就失效。
#include <cstdint>
#include <string>
#include <vector>

#include "abi/iface/info.h"
#include "detail/abi_bridge.hpp"
#include "error.hpp"

namespace mcdk {

    // 游戏生命周期状态。
    //
    // **判定只基于调试 IPC 的连接状态，不保证与游戏真实状态严格一致，但覆盖绝大多数场景。**
    enum class GameState {
        Unavailable, // 调试 IPC 未启用
        Loading,     // 进程已创建，从未握过手
        Menu,        // 曾握过手，现在没有客户端
        InWorld,     // 调试 IPC 有客户端
        Exited,
    };

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
        // 等价于 state == GameState::InWorld。要区分「还在加载」与「退回主菜单」看 state。
        bool      gameDebugReady = false;
        GameState state          = GameState::Unavailable;

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
            result.state          = fromAbi(raw.game_state);

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

        // 已连接的调试 IPC 客户端的对端端口。
        [[nodiscard]] std::vector<std::uint16_t> ipcClientPorts() const {
            if (!detail::ifaceHas(mTable, &mcdk_iface_info::get_ipc_clients)) {
                return {};
            }
            std::size_t count = 0;
            if (mTable->get_ipc_clients(mSelf, nullptr, 0, &count) != MCDK_OK && count == 0) {
                return {};
            }
            std::vector<std::uint16_t> ports(count);
            if (count == 0) {
                return ports;
            }
            if (mTable->get_ipc_clients(mSelf, ports.data(), ports.size(), &count) != MCDK_OK) {
                return {};
            }
            ports.resize(count);
            return ports;
        }

    private:
        [[nodiscard]] static GameState fromAbi(mcdk_game_state raw) noexcept {
            switch (raw) {
            case MCDK_GAME_LOADING:
                return GameState::Loading;
            case MCDK_GAME_MENU:
                return GameState::Menu;
            case MCDK_GAME_IN_WORLD:
                return GameState::InWorld;
            case MCDK_GAME_EXITED:
                return GameState::Exited;
            case MCDK_GAME_UNAVAILABLE:
            default:
                return GameState::Unavailable;
            }
        }

        mcdk_handle            mSelf  = 0;
        const mcdk_iface_info* mTable = nullptr;
    };

} // namespace mcdk
