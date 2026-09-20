// mcdk.info/1 的宿主实现。
// 本文件里的每个导出函数都必须经过 guard / guardVoid，没有例外
#include <cstring>
#include <filesystem>
#include <string>

#include <mcdk/plugin/abi/iface/info.h>
#include <mcdk/plugin_host/guard.hpp>
#include <mcdevtool/utils.h>

#include "../registry.hpp"

namespace mcdk::plugin_host::detail {

    namespace {

        [[nodiscard]] mcdk_str toAbi(std::string_view text) noexcept {
            mcdk_str out;
            out.ptr = text.data();
            out.len = text.size();
            return out;
        }
// 字符串字段按借用交付，所以指向的存储必须活过本次调用返回之后的「立即拷贝」
// 那一瞬。快照本身是调用时现算的，因此存进线程局部——同一线程的下一次
        struct InfoStrings {
            std::string mcpIp;
            std::string gameExePath;
            std::string projectRoot;
            std::string worldName;
            std::string worldFolderName;
            std::string worldRuntimePath;
            std::string worldSourcePath;
        };

        [[nodiscard]] InfoStrings& infoStrings() noexcept {
            thread_local InfoStrings storage;
            return storage;
        }

        mcdk_status MCDK_CALL infoGetSession(mcdk_handle self, mcdk_session_info* out_info) noexcept {
            return guard([&]() -> mcdk_status {
                if (out_info == nullptr) {
                    return MCDK_ERR_INVALID_ARGUMENT;
                }
                if (registry().find(self) == nullptr) {
                    return MCDK_ERR_INVALID_HANDLE;
                }
                // 调用方声明它认识到哪个字段为止；只写它认识的部分。
                const std::uint32_t declared = out_info->struct_size;
                if (declared < sizeof(mcdk_session_info)) {
                    // v1 只有一个版本的布局，短于它说明对方给错了 struct_size。
                    return MCDK_ERR_INVALID_ARGUMENT;
                }

                const auto binding = sessionBinding();
                auto&      strings = infoStrings();

                strings.mcpIp            = binding->facts.mcpIp;
                strings.gameExePath      = binding->facts.gameExePath;
                strings.worldName        = binding->facts.worldName;
                strings.worldFolderName  = binding->facts.worldFolderName;
                strings.worldRuntimePath = binding->facts.worldRuntimePath;
                strings.worldSourcePath  = binding->facts.worldSourcePath;
                // 项目根在任何阶段都是知道的，绑定之前也一样。
                strings.projectRoot = binding->facts.projectRoot.empty()
                                          ? MCDevTool::Utils::pathToGenericUtf8(std::filesystem::current_path())
                                          : binding->facts.projectRoot;

                mcdk_session_info info{};
                info.struct_size = static_cast<std::uint32_t>(sizeof(mcdk_session_info));
                info.mcdk_pid    = binding->facts.mcdkPid;
                info.game_pid    = binding->gamePid ? binding->gamePid->load(std::memory_order_relaxed) : 0u;
                info.game_ipc_port = binding->ipcServer ? binding->ipcServer->getPort() : std::uint16_t{0};
                info.mcp_port      = binding->facts.mcpPort;
                info.mcp_enabled   = binding->facts.mcpEnabled ? MCDK_TRUE : MCDK_FALSE;
                // 「游戏已进入世界」的判据就是调试 IPC 上已经有客户端连上来。
                info.game_debug_ready =
                    (binding->ipcServer && binding->ipcServer->getClientCount() > 0) ? MCDK_TRUE : MCDK_FALSE;

                info.mcp_ip             = toAbi(strings.mcpIp);
                info.game_exe_path      = toAbi(strings.gameExePath);
                info.project_root       = toAbi(strings.projectRoot);
                info.world_name         = toAbi(strings.worldName);
                info.world_folder_name  = toAbi(strings.worldFolderName);
                info.world_runtime_path = toAbi(strings.worldRuntimePath);
                info.world_source_path  = toAbi(strings.worldSourcePath);

                *out_info = info;
                return MCDK_OK;
            });
        }

        constexpr mcdk_iface_info kTable = {
            /* struct_size */ static_cast<uint32_t>(sizeof(mcdk_iface_info)),
            /* _reserved   */ 0u,
            /* get_session */ &infoGetSession,
        };

    } // namespace

    const mcdk_iface_info* infoTable() noexcept { return &kTable; }

} // namespace mcdk::plugin_host::detail
