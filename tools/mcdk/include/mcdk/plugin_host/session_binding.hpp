#pragma once

//
// 宿主运行期子系统向插件接口层的注入点。
//
// 接口 shim 是自由函数，签名里只有 `mcdk_handle self`，拿不到 `runtime::Session`。
// 所以运行期就绪后由 `launchGameExe` 把需要的东西绑进来，shim 从这里取。
//
// 这里**故意不引用 `runtime::Session`**，只持有它的几个成员：
//   - plugin_host 不必依赖 Session 的完整定义，避免把整棵运行期头文件树拖进来；
//   - 用 shared_ptr 共享所有权，插件线程在关停竞态中取到的对象不会是野指针。
//
// 绑定之前（REGISTER / CONFIG / WORLD 阶段）`mcdk.info` 仍然可用，只是游戏相关
// 字段全为 0——这正是 `game_pid == 0` 表示「游戏进程尚未创建」的由来。
//

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include <mcdevtool/debug.h>

#include <mcdk/log_buffer.hpp>

namespace mcdk::runtime {
    class McpToolRegistry;
}

namespace mcdk::plugin_host {

    // 会话期内不再变化的事实。
    struct SessionFacts {
        std::uint32_t mcdkPid    = 0;
        std::uint16_t mcpPort    = 0;
        bool          mcpEnabled = false;
        std::string   mcpIp;
        // 以下路径一律是 UTF-8 generic 形式（Utils::pathToGenericUtf8）。
        std::string gameExePath;
        std::string projectRoot;
        std::string worldName;
        std::string worldFolderName;
        std::string worldRuntimePath;
        std::string worldSourcePath; // 空 = 非玩法地图工程
    };

    struct SessionBinding {
        SessionFacts facts;
        // 游戏进程 id。0 表示尚未创建；由 Session::onGameProcessStarted 写入。
        std::shared_ptr<std::atomic<std::uint32_t>>       gamePid;
        std::shared_ptr<MCDevTool::Debug::DebugIPCServer> ipcServer;
        std::shared_ptr<LogBuffer>                        logBuffer;
        std::shared_ptr<LogBuffer>                        errBuffer;
        std::shared_ptr<runtime::McpToolRegistry>         mcpToolRegistry;
    };

    // 运行期子系统就绪后调用。多次调用以最后一次为准。
    void bindSession(SessionBinding binding);

    // 运行期子系统拆除前调用，释放上面那几个 shared_ptr。
    void unbindSession() noexcept;

} // namespace mcdk::plugin_host
