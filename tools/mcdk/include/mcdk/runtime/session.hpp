#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>

#include <mcdevtool/debug.h>

#include <mcdk/env.hpp>
#include <mcdk/host_bridge.hpp>
#include <mcdk/hotreload.hpp>
#include <mcdk/log_buffer.hpp>
#include <mcdk/mcp_server.hpp>
#include <mcdk/performance/profiler_runtime_owner.hpp>
#include <mcdk/runtime/game_lifecycle.hpp>
#include <mcdk/settings.hpp>
#include <mcdk/style_processor.hpp>

namespace mcdk::runtime {

    struct SessionOptions {
        // native profiler 组件与 mcdk 可执行文件同目录，用于定位它
        std::filesystem::path executableDirectory;
        // profiler 采样结果的落盘根目录
        std::filesystem::path profileStorageRoot;
    };

    // 一次游戏运行期内全部子系统的持有者。
    // 这些对象过去是 launchGameExe 的局部变量，函数之外无从访问。集中到这里之后，
    class Session {
    public:
        Session(const UserConfig& userConfig, HostBridgeConfig hostBridgeConfig, SessionOptions options);

        // 成员中的 watcher 任务与 Host Bridge 各自持有线程，均不可移动，故整体不可复制、不可移动。
        Session(const Session&)            = delete;
        Session& operator=(const Session&) = delete;
        Session(Session&&)                 = delete;
        Session& operator=(Session&&)      = delete;

        [[nodiscard]] std::shared_ptr<LogBuffer>&                  logBuffer() noexcept { return mLogBuffer; }
        [[nodiscard]] std::shared_ptr<LogBuffer>&                  errBuffer() noexcept { return mErrBuffer; }
        [[nodiscard]] std::shared_ptr<std::atomic<std::uint32_t>>& profilerGamePid() noexcept {
            return mProfilerGamePid;
        }
        [[nodiscard]] std::shared_ptr<MCDevTool::Debug::DebugIPCServer>&  ipcServer() noexcept { return mIpcServer; }
        [[nodiscard]] std::shared_ptr<performance::ProfilerRuntimeOwner>& profilerRuntime() noexcept {
            return mProfilerRuntime;
        }

        [[nodiscard]] MCPServer& mcpServer() noexcept { return mMcpServer; }

        // 宿主内唯一的一份游戏生命周期状态，Host Bridge 与插件宿主共用。
        [[nodiscard]] const std::shared_ptr<GameLifecycleTracker>& gameLifecycle() const noexcept {
            return mGameLifecycle;
        }

        // MCP 工具注册表。内置工具与插件工具共用，MCPServer::start() 时按表发布。
        [[nodiscard]] const std::shared_ptr<McpToolRegistry>& mcpToolRegistry() const noexcept {
            return mMcpServer.toolRegistry();
        }
        [[nodiscard]] PyReloadWatcherTask&       pyReloadTask() noexcept { return mPyReloadTask; }
        [[nodiscard]] UiReloadWatcherTask&       uiReloadTask() noexcept { return mUiReloadTask; }
        [[nodiscard]] ShaderReloadWatcherTask&   shaderReloadTask() noexcept { return mShaderReloadTask; }
        [[nodiscard]] MaterialReloadWatcherTask& materialReloadTask() noexcept { return mMaterialReloadTask; }
        [[nodiscard]] ParticleReloadWatcherTask& particleReloadTask() noexcept { return mParticleReloadTask; }
        [[nodiscard]] UserStyleProcessor&        styleProcessor() noexcept { return mStyleProcessor; }
        [[nodiscard]] HostBridgeTask&            hostBridgeTask() noexcept { return mHostBridgeTask; }

        // 游戏进程创建完成后调用，把进程 id 分发给需要它的子系统。
        void onGameProcessStarted(std::uint32_t minecraftPid);

        // 游戏进程退出后调用，按依赖关系逆序停止全部子系统。
        // 顺序是有约束的，改动前请阅读实现中的注释。
        void shutdown(std::uint32_t minecraftExitCode);

    private:
        // 声明顺序即初始化顺序：profiler 运行时依赖 IPC 服务与进程 id，必须排在它们之后。
        std::shared_ptr<LogBuffer>                         mLogBuffer;
        std::shared_ptr<LogBuffer>                         mErrBuffer;
        std::shared_ptr<std::atomic<std::uint32_t>>        mProfilerGamePid;
        std::shared_ptr<MCDevTool::Debug::DebugIPCServer>  mIpcServer;
        std::shared_ptr<performance::ProfilerRuntimeOwner> mProfilerRuntime;
        std::shared_ptr<GameLifecycleTracker>              mGameLifecycle = std::make_shared<GameLifecycleTracker>();

        MCPServer                 mMcpServer;
        PyReloadWatcherTask       mPyReloadTask;
        UiReloadWatcherTask       mUiReloadTask;
        ShaderReloadWatcherTask   mShaderReloadTask;
        MaterialReloadWatcherTask mMaterialReloadTask;
        ParticleReloadWatcherTask mParticleReloadTask;
        UserStyleProcessor        mStyleProcessor;
        HostBridgeTask            mHostBridgeTask;
    };

} // namespace mcdk::runtime
