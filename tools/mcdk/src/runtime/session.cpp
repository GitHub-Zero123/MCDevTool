#include <mcdk/runtime/session.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <utility>

#include <mcdk/ipc_code_execution.hpp>
#include <mcdk/performance/profiler_service_factory.hpp>
#include <mcdk/performance/profiler_types.hpp>

#include <mcdevtool/style.h>
#include <nlohmann/json.hpp>

namespace mcdk::runtime {

    Session::Session(const UserConfig& userConfig, HostBridgeConfig hostBridgeConfig, SessionOptions options)
    : mLogBuffer(std::make_shared<LogBuffer>(1000, 250)),
      mErrBuffer(std::make_shared<LogBuffer>(1000, 400)),
      mProfilerGamePid(std::make_shared<std::atomic<std::uint32_t>>(0)),
      mIpcServer(MCDevTool::Debug::createDebugServer()),
      mProfilerRuntime(
          std::make_shared<performance::ProfilerRuntimeOwner>([ipcServer       = mIpcServer,
                                                               profilerGamePid = mProfilerGamePid,
                                                               storageRoot     = std::move(options.profileStorageRoot),
                                                               executableDirectory =
                                                                   std::move(options.executableDirectory)] {
              return performance::createProfilerService({
                  .executeCode =
                      [ipcServer](std::string code, performance::ProfileTarget side, std::chrono::milliseconds timeout)
                      -> std::expected<nlohmann::json, performance::GameExecutionError> {
                      if (!ipcServer || ipcServer->getClientCount() == 0) {
                          return std::unexpected(
                              performance::GameExecutionError{
                                  .code      = "GAME_WORLD_NOT_READY",
                                  .message   = "Minecraft has not entered a world or the debug IPC is unavailable.",
                                  .retryable = true,
                              }
                          );
                      }
                      const bool isClient = side != performance::ProfileTarget::Server;
                      auto       value    = ipc_code_execution::requestCodeReturnValueJson(
                          ipcServer,
                          std::move(code),
                          isClient,
                          static_cast<std::uint32_t>(std::clamp<std::int64_t>(timeout.count(), 1, 120000))
                      );
                      if (value.is_object() && value.contains("error") && !value.contains("reason")) {
                          return std::unexpected(
                              performance::GameExecutionError{
                                  .code      = "GAME_EXECUTION_FAILED",
                                  .message   = value.value("error", "Game IPC execution failed."),
                                  .retryable = true,
                              }
                          );
                      }
                      return value;
                  },
                  .currentGameProcessId =
                      [profilerGamePid] { return profilerGamePid->load(std::memory_order_acquire); },
                  .storageRoot         = storageRoot,
                  .executableDirectory = executableDirectory,
                  .memoryIdleTimeout   = std::chrono::minutes(20),
              });
          })
      ),
      mMcpServer(userConfig.mcpServer),
      mStyleProcessor(0, userConfig.windowStyle),
      mHostBridgeTask(std::move(hostBridgeConfig)) {}

    void Session::onGameProcessStarted(std::uint32_t minecraftPid) {
        mProfilerGamePid->store(minecraftPid, std::memory_order_release);
        mStyleProcessor.setPid(static_cast<int>(minecraftPid));
        mMcpServer.setMinecraftProcessId(static_cast<int>(minecraftPid));
    }

    void Session::shutdown(std::uint32_t minecraftExitCode) {
        mHostBridgeTask.notifyMinecraftExited(minecraftExitCode);
        mProfilerGamePid->store(0, std::memory_order_release);

        // 停止热更新任务
        mPyReloadTask.safeExit();
        mUiReloadTask.safeExit();
        mShaderReloadTask.safeExit();
        mMaterialReloadTask.safeExit();
        mParticleReloadTask.safeExit();
        // Stop new MCP calls before tearing down the profiler runtime they invoke.
        mMcpServer.stop();
        // Profiler cleanup must finish while the game IPC executor is still available.
        mProfilerRuntime->shutdown();
        mIpcServer->safeExit();
        mHostBridgeTask.safeExit();
        // 停止样式处理器
        mStyleProcessor.safeExit();
    }

} // namespace mcdk::runtime
