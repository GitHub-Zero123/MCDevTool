// MCDK
#include <console_output.hpp>
#include <game_process.hpp>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>


// mcdk modules
#include <config.hpp>
#include <console.hpp>
#include <env.hpp>
#include <hotreload.hpp>
#include <host_bridge.hpp>
#include <ipc_code_execution.hpp>
#include <jsonui_reload_support.hpp>
#include <level.hpp>
#include <log_buffer.hpp>
#include <material_reload_support.hpp>
#include <mcp_server.hpp>
#include <mod_dir_config.hpp>
#include <mod_register.hpp>
#include <particle_reload_support.hpp>
#include <shader_reload_support.hpp>
#include <style_processor.hpp>
#include <utils.hpp>
#include <world_project.hpp>


// mcdevtool api
#include <mcdevtool/addon.h>
#include <mcdevtool/utils.h>
#include <mcdevtool/debug.h>
#include <mcdevtool/env.h>
#include <mcdevtool/level.h>
#include <mcdevtool/style.h>
#include <nlohmann/json.hpp>


// 默认使用"\n"而非std::endl输出日志，避免大量log的性能开销
#define _MCDEV_LOG_OUTPUT_ENDL "\n"

#ifdef MCDEV_LOG_FORCE_FLUSH_ENDL
// 强制使用std::endl
#undef _MCDEV_LOG_OUTPUT_ENDL
#define _MCDEV_LOG_OUTPUT_ENDL std::endl
#endif

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {
    class UniqueHandle {
    public:
        UniqueHandle() = default;
        explicit UniqueHandle(HANDLE handle) noexcept : mHandle(handle) {}
        ~UniqueHandle() { reset(); }

        UniqueHandle(const UniqueHandle&)            = delete;
        UniqueHandle& operator=(const UniqueHandle&) = delete;

        UniqueHandle(UniqueHandle&& other) noexcept : mHandle(other.release()) {}

        UniqueHandle& operator=(UniqueHandle&& other) noexcept {
            if (this != &other) {
                reset(other.release());
            }
            return *this;
        }

        [[nodiscard]] HANDLE get() const noexcept { return mHandle; }

        HANDLE* receive() noexcept {
            reset();
            return &mHandle;
        }

        HANDLE release() noexcept {
            const HANDLE handle = mHandle;
            mHandle             = nullptr;
            return handle;
        }

        void reset(HANDLE handle = nullptr) noexcept {
            if (mHandle != nullptr && mHandle != INVALID_HANDLE_VALUE) {
                CloseHandle(mHandle);
            }
            mHandle = handle;
        }

    private:
        HANDLE mHandle = nullptr;
    };
} // namespace
#endif

using mcdk::printColoredAtomic;
using mcdk::UserModDirConfig;
using mcdk::UserStyleProcessor;
using ConsoleColor = mcdk::ConsoleColor;

static std::mutex g_consoleMutex;

// 线程安全彩色输出
void mcdk::printColoredAtomic(const std::string& msg, ConsoleColor color) {
    std::lock_guard<std::mutex> lk(g_consoleMutex);
    HANDLE                      hConsole = GetStdHandle(STD_OUTPUT_HANDLE);

    if (hConsole == INVALID_HANDLE_VALUE) {
        std::cout << msg << _MCDEV_LOG_OUTPUT_ENDL;
        return;
    }

    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(hConsole, &info)) {
        std::cout << msg << _MCDEV_LOG_OUTPUT_ENDL;
        return;
    }

    WORD attr = 0;

    switch (color) {
    case ConsoleColor::Green:
        attr = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        break;
    case ConsoleColor::Red:
        attr = FOREGROUND_RED | FOREGROUND_INTENSITY;
        break;
    case ConsoleColor::Blue:
        attr = FOREGROUND_BLUE | FOREGROUND_INTENSITY;
        break;
    case ConsoleColor::Yellow:
        attr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        break;
    case ConsoleColor::Cyan:
        attr = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
        break;
    case ConsoleColor::Magenta:
        attr = FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
        break;
    case ConsoleColor::White:
        attr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
        break;
    case ConsoleColor::Black:
        attr = 0;
        break;
    case ConsoleColor::Gray:
        // 亮灰 = RGB，但不加高亮
        attr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
        break;
    case ConsoleColor::DarkGray:
        // 深灰 = 只加高亮，不加 RGB
        attr = FOREGROUND_INTENSITY;
        break;
    default:
        break;
    }

    if (color != ConsoleColor::Default) {
        SetConsoleTextAttribute(hConsole, attr);
    }

    std::cout << msg << _MCDEV_LOG_OUTPUT_ENDL;

    if (color == ConsoleColor::Default) {
        return;
    }
    // 恢复原色
    SetConsoleTextAttribute(hConsole, info.wAttributes);
}

void mcdk::printStartupLogo(bool pluginEnv) {
    std::cout << _MCDEV_LOG_OUTPUT_ENDL;
    std::cout << "  ███╗   ███╗ ██████╗ ██████╗ ██╗  ██╗\n"
              << "  ████╗ ████║██╔════╝ ██╔══██╗██║ ██╔╝\n"
              << "  ██╔████╔██║██║      ██║  ██║█████╔╝\n"
              << "  ██║╚██╔╝██║██║      ██║  ██║██╔═██╗\n"
              << "  ██║ ╚═╝ ██║╚██████╗ ██████╔╝██║  ██╗\n"
              << "  ╚═╝     ╚═╝ ╚═════╝ ╚═════╝ ╚═╝  ╚═╝" << _MCDEV_LOG_OUTPUT_ENDL;
    printColoredAtomic("  Minecraft Creator Development Kit", ConsoleColor::DarkGray);
    if (pluginEnv) {
        printColoredAtomic("  Kid Studio Core Tool · VSCode Extension: Dofes, Zero123", ConsoleColor::DarkGray);
    } else {
        printColoredAtomic("  Kid Studio Core Tool", ConsoleColor::DarkGray);
    }
    std::cout << _MCDEV_LOG_OUTPUT_ENDL;
}

// 进程buffer行处理
static void processBufferAppend(
    std::string&                                   lineBuf,
    const char*                                    buf,
    size_t                                         len,
    bool                                           filterPython,
    const std::function<void(std::string)>&        processLine
) {
    lineBuf.append(buf, len);

    size_t consumed = 0;
    size_t pos      = 0;
    while ((pos = lineBuf.find('\n', consumed)) != std::string::npos) {
        std::string line = lineBuf.substr(consumed, pos - consumed);
        consumed         = pos + 1;

        // 去除行尾可能存在的 '\r'
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        // 过滤：若启用只保留 [Python] 则丢弃其它
        if (filterPython && line.find("[Python] ") == std::string::npos) continue;

        processLine(std::move(line));
    }
    if (consumed != 0) {
        // Remove all completed lines once instead of shifting the string after every newline.
        lineBuf.erase(0, consumed);
    }
}

#ifdef _WIN32

// pipe线程处理函数
static void
readPipeThread(HANDLE hPipe, bool filterPython, const std::function<void(std::string)>& processLine) {
    constexpr DWORD   BUFSZ = 4096;
    std::string       lineBuf;
    std::vector<char> buffer(BUFSZ);

    while (true) {
        DWORD bytesRead = 0;
        BOOL  ok        = ReadFile(hPipe, buffer.data(), BUFSZ, &bytesRead, NULL);
        if (!ok) {
            DWORD err = GetLastError();
            // ERROR_BROKEN_PIPE (109) 表示写端已关闭并读尽
            if (err == ERROR_BROKEN_PIPE) {
                // 处理残留并退出
                if (!lineBuf.empty()) {
                    // 没有换行但还有内容，作为最后一行处理
                    std::string lastLine = lineBuf;
                    if (!lastLine.empty() && lastLine.back() == '\r') lastLine.pop_back();
                    if (!(filterPython && lastLine.find("[Python] ") == std::string::npos)) {
                        processLine(std::move(lastLine));
                    }
                    lineBuf.clear();
                }
                break;
            } else {
                // 其它错误直接退出
                break;
            }
        }

        if (bytesRead == 0) {
            // 管道关闭或无数据（通常与 ERROR_BROKEN_PIPE 一致）
            // 处理残留并退出
            if (!lineBuf.empty()) {
                std::string lastLine = lineBuf;
                if (!lastLine.empty() && lastLine.back() == '\r') lastLine.pop_back();
                if (!(filterPython && lastLine.find("[Python] ") == std::string::npos)) {
                    processLine(std::move(lastLine));
                }
                lineBuf.clear();
            }
            break;
        }

        // 追加并按行处理（会把完整行交给 processLine，残留留在 lineBuf）
        processBufferAppend(lineBuf, buffer.data(), bytesRead, filterPython, processLine);
    }
}

namespace {
    class PipeReaderThreads {
    public:
        ~PipeReaderThreads() {
            // On exceptional exit, cancel blocking ReadFile calls before joining so thread destruction cannot terminate.
            cancelAndJoin(mStdout);
            cancelAndJoin(mStderr);
        }

        void start(
            HANDLE                                  stdoutPipe,
            HANDLE                                  stderrPipe,
            bool                                    filterPython,
            const std::function<void(std::string)>& stdoutCallback,
            const std::function<void(std::string)>& stderrCallback
        ) {
            mStdout = std::thread(readPipeThread, stdoutPipe, filterPython, stdoutCallback);
            mStderr = std::thread(readPipeThread, stderrPipe, filterPython, stderrCallback);
        }

        void join() {
            if (mStdout.joinable()) mStdout.join();
            if (mStderr.joinable()) mStderr.join();
        }

    private:
        static void cancelAndJoin(std::thread& thread) noexcept {
            if (!thread.joinable()) return;
            CancelSynchronousIo(thread.native_handle());
            try {
                thread.join();
            } catch (...) {
                // A destructor must not terminate while unwinding; detaching is the last-resort valid thread state.
                thread.detach();
            }
        }

        std::thread mStdout;
        std::thread mStderr;
    };
} // namespace

// 尝试附加调试器到指定进程
static void debuggerAttachToProcess(DWORD pid, int port) {
    // 执行cmd调用mcdbg.exe附加（如果失败则输出错误信息）
    std::string cmd;
    cmd.reserve(48);

    cmd.append("mcdbg.exe --pid ");
    cmd.append(std::to_string(pid));
    cmd.append(" --port ");
    cmd.append(std::to_string(port));
    STARTUPINFOA        si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        std::cerr << "警告：无法启动mcdbg.exe附加调试器，请确保其在环境变量路径中。" << _MCDEV_LOG_OUTPUT_ENDL;
        return;
    }
    // The debugger process continues independently; only its duplicated handles belong to this launcher.
    UniqueHandle debuggerProcess(pi.hProcess);
    UniqueHandle debuggerThread(pi.hThread);
    std::cout << "调试器已启动，正在附加到进程PID：" << pid << " 端口：" << port << " ..." << _MCDEV_LOG_OUTPUT_ENDL;
}

// 将utf8的string转换为utf16的wstring
static std::wstring convertUtf8ToUtf16(const std::string& utf8Str) {
    if (utf8Str.empty()) {
        return std::wstring();
    }
    int wideCharLen = MultiByteToWideChar(CP_UTF8, 0, utf8Str.data(), static_cast<int>(utf8Str.size()), nullptr, 0);
    if (wideCharLen == 0) {
        throw std::runtime_error("Failed to convert UTF-8 to UTF-16.");
    }
    std::wstring utf16Str(wideCharLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8Str.data(), static_cast<int>(utf8Str.size()), &utf16Str[0], wideCharLen);
    return utf16Str;
}

// 生成新的环境变量w字符串（继承当前环境变量并添加新变量）
static bool environmentVariableNameEquals(std::wstring_view entry, std::wstring_view expectedName) {
    const auto separator = entry.find(L'=');
    if (separator == std::wstring_view::npos || separator == 0) {
        return false;
    }
    const auto name = entry.substr(0, separator);
    return CompareStringOrdinal(
               name.data(),
               static_cast<int>(name.size()),
               expectedName.data(),
               static_cast<int>(expectedName.size()),
               TRUE
           ) == CSTR_EQUAL;
}

static std::wstring createNewEnvironmentBlock(const std::wstring& newVar, const std::wstring& newValue) {
    // 获取当前环境变量块
    auto envBlock = std::unique_ptr<wchar_t, decltype(&FreeEnvironmentStringsW)>(
        GetEnvironmentStringsW(),
        FreeEnvironmentStringsW
    );
    if (!envBlock) {
        throw std::runtime_error("Failed to get current environment strings.");
    }

    std::wstring newEnvBlock;
    // 复制现有环境变量
    // The API block now releases automatically even if growing newEnvBlock throws.
    LPWCH current = envBlock.get();
    while (*current) {
        std::wstring varLine(current);
        const bool isHostBridgeSecret = environmentVariableNameEquals(varLine, L"MCDEV_HOST_PORT")
                                     || environmentVariableNameEquals(varLine, L"MCDEV_HOST_TOKEN");
        const bool isReplacedVariable = !newVar.empty() && environmentVariableNameEquals(varLine, newVar);
        if (!isHostBridgeSecret && !isReplacedVariable) {
            newEnvBlock += varLine + L'\0';
        }
        current     += varLine.size() + 1;
    }

    // 添加新的环境变量
    if (!newVar.empty()) {
        newEnvBlock += newVar + L'=' + newValue + L'\0';
    }

    // 结束环境变量块
    newEnvBlock += L'\0';

    return newEnvBlock;
}

// 启动游戏可执行文件
void mcdk::launchGameExe(
    const std::filesystem::path&                   exePath,
    std::string_view                               config,
    const mcdk::UserConfig&                        userConfig,
    const std::vector<UserModDirConfig>*           modDirList,
    const std::vector<MCDevTool::Addon::PackInfo>* linkedPacks
) {
    const bool  autoHotReload          = userConfig.hotReload.mods;
    const bool  autoHotReloadUi        = userConfig.hotReload.ui;
    const bool  autoHotReloadShaders   = userConfig.hotReload.shaders;
    const bool  autoHotReloadMaterials = userConfig.hotReload.materials;
    const bool  autoHotReloadParticles = userConfig.hotReload.particles;
    const auto& mcpServerConfig        = userConfig.mcpServer;
    auto        hostBridgeConfig       = mcdk::getEnvHostBridgeConfig();
    const bool  hostBridgeConfigured   = hostBridgeConfig.configured;
    auto        hotReloadDirs =
        modDirList != nullptr ? UserModDirConfig::toPathList(*modDirList) : std::vector<std::filesystem::path>();
    auto hotReloadUiDirs         = autoHotReloadUi && linkedPacks != nullptr
                                     ? UserModDirConfig::collectHotReloadResourceSubdirPaths(*linkedPacks, "ui")
                                     : std::vector<std::filesystem::path>();
    auto hotReloadShaderDirs     = autoHotReloadShaders && linkedPacks != nullptr
                                     ? UserModDirConfig::collectHotReloadResourceSubdirPaths(*linkedPacks, "shaders")
                                     : std::vector<std::filesystem::path>();
    auto hotReloadMaterialDirs   = autoHotReloadMaterials && linkedPacks != nullptr
                                     ? UserModDirConfig::collectHotReloadResourceSubdirPaths(*linkedPacks, "materials")
                                     : std::vector<std::filesystem::path>();
    auto hotReloadParticleDirs   = autoHotReloadParticles && linkedPacks != nullptr
                                     ? UserModDirConfig::collectHotReloadResourceSubdirPaths(*linkedPacks, "particles")
                                     : std::vector<std::filesystem::path>();
    bool enablePyHotReload       = autoHotReload && !hotReloadDirs.empty();
    bool enableUiHotReload       = autoHotReloadUi && !hotReloadUiDirs.empty();
    bool enableShaderHotReload   = autoHotReloadShaders && !hotReloadShaderDirs.empty();
    bool enableMaterialHotReload = autoHotReloadMaterials && !hotReloadMaterialDirs.empty();
    bool enableParticleHotReload = autoHotReloadParticles && !hotReloadParticleDirs.empty();
    bool enableAnyHotReload = enablePyHotReload || enableUiHotReload || enableShaderHotReload || enableMaterialHotReload
                           || enableParticleHotReload;
    bool  enableIPC     = mcpServerConfig.enabled || enableAnyHotReload || hostBridgeConfig.enabled;
    bool  needLogBuffer = false;
    void* lpEnvironment = nullptr;

    auto ipcServer = MCDevTool::Debug::createDebugServer();
    auto logBuffer = std::make_shared<mcdk::LogBuffer>(1000, 250);
    auto errBuffer = std::make_shared<mcdk::LogBuffer>(1000, 400);
    auto mcpServer = mcdk::MCPServer(mcpServerConfig);
    if (mcpServerConfig.enabled) {
        // 若启用MCP服务器将自动启用IPC调试功能
        enableIPC     = true;
        needLogBuffer = true;
        printColoredAtomic(
            "[MCDK] MCP Server " + mcpServerConfig.serverIp + ":" + std::to_string(mcpServerConfig.serverPort),
            ConsoleColor::Green
        );
        mcpServer.setLogBuffer(logBuffer);
        mcpServer.setErrBuffer(errBuffer);

        // 代码执行Handler
        mcpServer.setCodeExecuteHandler(
            [ipcServer](const std::string& code, bool isClient, bool directReturn) -> nlohmann::json {
                auto makeTextResult = [](bool isError, const std::string& text) -> nlohmann::json {
                    return nlohmann::json{
                        {"isError", isError},
                        {"content", nlohmann::json::array({{{"type", "text"}, {"text", text}}})}
                    };
                };

                if (ipcServer->getClientCount() == 0) {
                    return makeTextResult(
                        true,
                        "Code execution failed. The player may not be in the game or the target is unavailable."
                    );
                }

                if (!directReturn) {
                    bool success = ipcServer->sendMessage(
                        isClient ? 3 : 4, // 3 = CLIENT_CODE_EXECUTE, 4 = SERVER_CODE_EXECUTE
                        code
                    ); // CODE EXECUTE
                    if (!success) {
                        return makeTextResult(
                            true,
                            "Code execution failed. The player may not be in the game or the target is unavailable."
                        );
                    }
                    return makeTextResult(
                        false,
                        "Code executed successfully on the target side. Please use get_latest_logs to observe the "
                        "execution result."
                    );
                }

                nlohmann::json params = {{"code", code}, {"is_client", isClient}};
                auto           result = ipcServer->requestJsonValue("execute_code", std::move(params), 10000);
                if (!result.success) {
                    return makeTextResult(true, "Code execution failed: " + result.errorMessage);
                }

                // Reuse the response DOM parsed for IPC id routing and avoid parsing large return values twice.
                auto response = result.responseValue
                              ? std::move(*result.responseValue)
                              : nlohmann::json::parse(result.responseJson, nullptr, false);
                if (response.is_discarded() || !response.is_object()) {
                    return makeTextResult(true, "Code execution returned invalid JSON: " + result.responseJson);
                }
                // The response DOM is retained, so release the duplicate wire payload before formatting its result.
                result.responseJson.clear();
                if (!response.value("ok", false)) {
                    std::string message = response.dump();
                    if (response.contains("error")) {
                        const auto& error = response["error"];
                        if (error.is_object() && error.contains("message")) {
                            message = error.value("message", message);
                        }
                    }
                    return makeTextResult(true, "Code execution failed: " + message);
                }

                nlohmann::json payload = nlohmann::json::object();
                if (response.contains("result")) {
                    // This response DOM is exclusively owned here, so move a potentially large result subtree.
                    payload = std::move(response["result"]);
                }
                std::ostringstream text;
                text << "Code executed successfully on " << (isClient ? "client" : "server") << ".";
                if (payload.is_object()) {
                    if (payload.contains("return_type")) {
                        text << "\nReturn type: " << payload.value("return_type", "unknown");
                    }
                    if (payload.contains("return_repr")) {
                        text << "\nReturn repr: " << payload.value("return_repr", "");
                    }
                    if (payload.contains("return_value")) {
                        text << "\nReturn value JSON: " << payload["return_value"].dump(2);
                    }
                } else {
                    text << "\nReturn value JSON: " << payload.dump(2);
                }
                return makeTextResult(false, text.str());
            }
        );

        // 重载游戏，reloadAddons=true 时同时重载 Addon 数据
        mcpServer.setReloadGameHandler([ipcServer, logBuffer, errBuffer](bool reloadAddons) -> bool {
            if (ipcServer->getClientCount() == 0) {
                return false; // 没有连接的客户端，无法执行
            }
            const int messageId = reloadAddons ? 8 : 5; // 8 = ADDON AND GAME RELOAD, 5 = GAME RELOAD
            if (!ipcServer->sendMessage(messageId)) {
                return false;
            }
            logBuffer->clear();
            errBuffer->clear();
            return true;
        });

        // 触发游戏窗口原生 Ctrl+R UI definition 热重载
        mcpServer.setReloadUiHandler([&mcpServer]() -> bool {
            const int pid = mcpServer.getMinecraftProcessId();
            if (pid <= 0) {
                return false;
            }
            return MCDevTool::Style::triggerMinecraftUiReloadShortcut(pid);
        });
        // Publish the MCP server only after every buffer and callback has been configured.
        mcpServer.start();
    }
    mcdk::PyReloadWatcherTask       pyReloadTask;
    mcdk::UiReloadWatcherTask       uiReloadTask;
    mcdk::ShaderReloadWatcherTask   shaderReloadTask;
    mcdk::MaterialReloadWatcherTask materialReloadTask;
    mcdk::ParticleReloadWatcherTask particleReloadTask;
    mcdk::UserStyleProcessor        styleProcessor(0, userConfig.windowStyle);
    mcdk::HostBridgeTask            hostBridgeTask(std::move(hostBridgeConfig));
    const bool                      debugCapabilityEnabled = userConfig.includeDebugMod && enableIPC;

    auto mustBindHostMethod = [](std::expected<void, mcdk::RpcBindError> result) {
        if (!result) {
            throw std::runtime_error("Failed to register Host Bridge game RPC method");
        }
    };
    auto invalidHostParams = [](std::string detail) -> mcdk::RpcResult {
        return std::unexpected(mcdk::RpcError{
            .code    = -32602,
            .message = "Invalid params",
            .data    = {{"code", "INVALID_PARAMS"}, {"detail", std::move(detail)}},
        });
    };
    auto gameWorldNotReady = []() -> mcdk::RpcResult {
        return std::unexpected(mcdk::RpcError{
            .code    = -32011,
            .message = "Minecraft has not entered a world",
            .data    = {{"code", "GAME_WORLD_NOT_READY"}, {"retryable", true}},
        });
    };

    mcdk::RpcMethodOptions ipcStatusOptions;
    ipcStatusOptions.execution        = mcdk::RpcExecutionPolicy::Inline;
    ipcStatusOptions.gameAvailability = mcdk::GameAvailability::None;
    ipcStatusOptions.timeout          = std::chrono::seconds(2);
    ipcStatusOptions.maxConcurrency   = 1;
    mustBindHostMethod(hostBridgeTask.registry().bindRaw(
        {
            .name         = "game/ipc/is-ready",
            .paramsSchema = {{"type", "object"}},
            .resultSchema = {
                {"type", "object"},
                {"required", {"ready", "debugCapabilityEnabled", "clientCount"}},
            },
        },
        ipcStatusOptions,
        [ipcServer, debugCapabilityEnabled](const mcdk::RpcContext&, const nlohmann::json& params) -> mcdk::RpcResult {
            if (!params.is_object()) {
                return std::unexpected(mcdk::RpcError{
                    .code    = -32602,
                    .message = "Invalid params",
                    .data    = {{"code", "INVALID_PARAMS"}, {"detail", "params must be an object"}},
                });
            }
            const auto clientCount = ipcServer->getClientCount();
            return nlohmann::json{
                {"ready", debugCapabilityEnabled && clientCount > 0},
                {"debugCapabilityEnabled", debugCapabilityEnabled},
                {"clientCount", clientCount},
            };
        }
    ));

    mcdk::RpcMethodOptions gameMethodOptions;
    gameMethodOptions.modes            = mcdk::RpcMode::Request | mcdk::RpcMode::Notification;
    gameMethodOptions.execution        = mcdk::RpcExecutionPolicy::GameSerial;
    gameMethodOptions.gameAvailability = mcdk::GameAvailability::InWorld;
    gameMethodOptions.timeout          = std::chrono::seconds(10);
    gameMethodOptions.maxConcurrency   = 1;

    mustBindHostMethod(hostBridgeTask.registry().bindRaw(
        {
            .name = "game/code/execute",
            .paramsSchema = {
                {"type", "object"},
                {"required", {"code"}},
                {"properties", {{"code", {{"type", "string"}}}, {"isClient", {{"type", "boolean"}}}}},
            },
            .resultSchema = nullptr,
        },
        gameMethodOptions,
        [ipcServer, invalidHostParams, gameWorldNotReady](
            const mcdk::RpcContext& context,
            const nlohmann::json&   params
        ) -> mcdk::RpcResult {
            if (!params.is_object() || !params.contains("code") || !params["code"].is_string()) {
                return invalidHostParams("code must be a string");
            }
            if (params.contains("isClient") && !params["isClient"].is_boolean()) {
                return invalidHostParams("isClient must be a boolean");
            }

            const auto& code     = params["code"].get_ref<const std::string&>();
            const bool  isClient = params.value("isClient", true);
            if (context.notification) {
                if (!ipcServer->sendMessage(isClient ? 3 : 4, code)) {
                    return gameWorldNotReady();
                }
                return nlohmann::json{{"accepted", true}};
            }

            auto ipcResult = ipcServer->requestJsonValue(
                "execute_code",
                {{"code", code}, {"is_client", isClient}},
                10000
            );
            if (!ipcResult.success) {
                if (ipcResult.timeout) {
                    return std::unexpected(mcdk::RpcError{
                        .code    = -32014,
                        .message = "Game IPC handler timed out",
                        .data    = {{"code", "HANDLER_TIMEOUT"}},
                    });
                }
                return gameWorldNotReady();
            }

            auto response = ipcResult.responseValue
                          ? std::move(*ipcResult.responseValue)
                          : nlohmann::json::parse(ipcResult.responseJson, nullptr, false);
            if (response.is_discarded() || !response.is_object()) {
                return std::unexpected(mcdk::RpcError{
                    .code    = -32603,
                    .message = "Game IPC returned invalid JSON",
                    .data    = {{"code", "INTERNAL_ERROR"}},
                });
            }
            if (!response.value("ok", false)) {
                std::string message  = "Python code execution failed";
                std::string gameCode = "execute_code_error";
                const auto  gameError = response.find("error");
                if (gameError != response.end() && gameError->is_object()) {
                    message  = gameError->value("message", message);
                    gameCode = gameError->value("code", gameCode);
                }
                return std::unexpected(mcdk::RpcError{
                    .code    = -32100,
                    .message = std::move(message),
                    .data    = {
                        {"code", "PYTHON_EXECUTION_FAILED"},
                        {"gameCode", std::move(gameCode)},
                        {"side", isClient ? "client" : "server"},
                    },
                });
            }

            auto result = response.find("result");
            if (result == response.end() || !result->is_object() || !result->contains("return_value")) {
                return std::unexpected(mcdk::RpcError{
                    .code    = -32603,
                    .message = "Game IPC response has no return value",
                    .data    = {{"code", "INTERNAL_ERROR"}},
                });
            }

            auto cleanResult = std::move((*result)["return_value"]);
            if (cleanResult.is_string()) {
                const auto& serialized = cleanResult.get_ref<const std::string&>();
                auto nested = nlohmann::json::parse(serialized, nullptr, false);
                if (!nested.is_discarded()) {
                    return nested;
                }
            }
            return cleanResult;
        }
    ));

    mustBindHostMethod(hostBridgeTask.registry().bindRaw(
        {
            .name = "game/reload",
            .paramsSchema = {
                {"type", "object"},
                {"properties", {{"addons", {{"type", "boolean"}}}}},
            },
            .resultSchema = {{"type", "object"}},
        },
        gameMethodOptions,
        [ipcServer, logBuffer, errBuffer, invalidHostParams, gameWorldNotReady](
            const mcdk::RpcContext&,
            const nlohmann::json& params
        ) -> mcdk::RpcResult {
            if (!params.is_object() || (params.contains("addons") && !params["addons"].is_boolean())) {
                return invalidHostParams("addons must be a boolean");
            }
            const bool reloadAddons = params.value("addons", false);
            if (!ipcServer->sendMessage(reloadAddons ? 8 : 5)) {
                return gameWorldNotReady();
            }
            logBuffer->clear();
            errBuffer->clear();
            return nlohmann::json{{"accepted", true}, {"addons", reloadAddons}};
        }
    ));
    pyReloadTask.setHotReloadAction([ipcServer](const mcdk::ReloadNames& targetPaths) {
        ipcServer->sendMessage(2, nlohmann::json(targetPaths).dump()); // FAST RELOAD
    });
    uiReloadTask.setUiHotReloadAction([ipcServer, &mcpServer]() {
        if (ipcServer->getClientCount() == 0) {
            printColoredAtomic(
                "[HotReload] UI hot reload skipped: IPC is not connected. The player may not be in game.",
                ConsoleColor::Yellow
            );
            return;
        }

        const int pid = mcpServer.getMinecraftProcessId();
        if (pid <= 0) {
            printColoredAtomic(
                "[HotReload] UI hot reload skipped: Minecraft process id is unavailable.",
                ConsoleColor::Red
            );
            return;
        }

        auto prepare = mcdk::ipc_code_execution::requestClientCodeReturnValueJson(
            ipcServer,
            mcdk::jsonui_reload_support::buildPreparePreserveModUiPythonCode(),
            10000
        );
        if (!prepare.is_object() || !prepare.value("ok", false)) {
            printColoredAtomic(
                "[HotReload] UI hot reload skipped: failed to freeze ModSDK UI snapshot. " + prepare.dump(),
                ConsoleColor::Red
            );
            return;
        }

        if (MCDevTool::Style::triggerMinecraftUiReloadShortcut(pid)) {
            printColoredAtomic(
                "[HotReload] UI hot reload triggered for resource-pack ui json changes.",
                ConsoleColor::Green
            );
            return;
        }

        auto restore = mcdk::ipc_code_execution::requestClientCodeReturnValueJson(
            ipcServer,
            mcdk::jsonui_reload_support::buildRestorePreservedModUiPythonCode(),
            10000
        );
        printColoredAtomic(
            "[HotReload] UI hot reload failed to trigger Ctrl+R; rollback restore result: " + restore.dump(),
            ConsoleColor::Red
        );
    });
    shaderReloadTask.setShaderHotReloadAction([ipcServer](const mcdk::ReloadNames& shaderNames) {
        if (ipcServer->getClientCount() == 0) {
            printColoredAtomic(
                "[HotReload] Shader hot reload skipped: IPC is not connected. The player may not be in game.",
                ConsoleColor::Yellow
            );
            return;
        }

        auto result = mcdk::ipc_code_execution::requestClientCodeReturnValueJson(
            ipcServer,
            mcdk::shader_reload_support::buildReloadShadersPythonCode(shaderNames, true),
            60000
        );
        if (!result.is_object() || !result.value("ok", false)) {
            printColoredAtomic("[HotReload] Shader hot reload failed: " + result.dump(), ConsoleColor::Red);
            return;
        }
        const auto attempted = result.value("attempted", 0);
        const auto reloaded  = result.value("reloaded", 0);
        const auto failed    = result.value("failed", nlohmann::json::array());
        if (!failed.empty()) {
            printColoredAtomic(
                "[HotReload] Shader hot reload finished with failures: " + result.dump(),
                ConsoleColor::Yellow
            );
            return;
        }
        printColoredAtomic(
            "[HotReload] Shader hot reload finished: " + std::to_string(reloaded) + "/" + std::to_string(attempted)
                + " shader(s) reloaded.",
            ConsoleColor::Green
        );
    });
    materialReloadTask.setMaterialHotReloadAction([ipcServer](const mcdk::ReloadNames& materialPaths) {
        if (ipcServer->getClientCount() == 0) {
            printColoredAtomic(
                "[HotReload] Material hot reload skipped: IPC is not connected. The player may not be in game.",
                ConsoleColor::Yellow
            );
            return;
        }

        auto result = mcdk::ipc_code_execution::requestClientCodeReturnValueJson(
            ipcServer,
            mcdk::material_reload_support::buildReloadMaterialsPythonCode(materialPaths, true),
            60000
        );
        if (!result.is_object() || !result.value("ok", false)) {
            if (result.is_object() && result.value("unsupported", false)) {
                printColoredAtomic(
                    "[HotReload] Material hot reload is not supported by this MC version. Please update to MC 3.9 or "
                    "newer.",
                    ConsoleColor::Yellow
                );
                return;
            }
            printColoredAtomic("[HotReload] Material hot reload failed: " + result.dump(), ConsoleColor::Red);
            return;
        }
        const auto attempted = result.value("attempted", 0);
        const auto reloaded  = result.value("reloaded", 0);
        const auto failed    = result.value("failed", nlohmann::json::array());
        if (!failed.empty()) {
            printColoredAtomic(
                "[HotReload] Material hot reload finished with failures: " + result.dump(),
                ConsoleColor::Yellow
            );
            return;
        }
        printColoredAtomic(
            "[HotReload] Material hot reload finished: " + std::to_string(reloaded) + "/" + std::to_string(attempted)
                + " material file(s) reloaded.",
            ConsoleColor::Green
        );
    });
    particleReloadTask.setParticleHotReloadAction([ipcServer](const mcdk::ReloadNames& particlePaths) {
        if (ipcServer->getClientCount() == 0) {
            printColoredAtomic(
                "[HotReload] Particle hot reload skipped: IPC is not connected. The player may not be in game.",
                ConsoleColor::Yellow
            );
            return;
        }

        auto result = mcdk::ipc_code_execution::requestClientCodeReturnValueJson(
            ipcServer,
            mcdk::particle_reload_support::buildReloadParticlesPythonCode(particlePaths),
            60000
        );
        if (!result.is_object() || !result.value("ok", false)) {
            if (result.is_object() && result.value("unsupported", false)) {
                printColoredAtomic(
                    "[HotReload] Particle hot reload is not supported by this MC version.",
                    ConsoleColor::Yellow
                );
                return;
            }
            printColoredAtomic("[HotReload] Particle hot reload failed: " + result.dump(), ConsoleColor::Red);
            return;
        }
        const auto attempted = result.value("attempted", 0);
        const auto reloaded  = result.value("reloaded", 0);
        const auto failed    = result.value("failed", nlohmann::json::array());
        if (!failed.empty()) {
            printColoredAtomic(
                "[HotReload] Particle hot reload finished with failures: " + result.dump(),
                ConsoleColor::Yellow
            );
            return;
        }
        printColoredAtomic(
            "[HotReload] Particle hot reload finished: " + std::to_string(reloaded) + "/" + std::to_string(attempted)
                + " particle file(s) reloaded.",
            ConsoleColor::Green
        );
    });
    pyReloadTask.setOutputCallback(printColoredAtomic);
    uiReloadTask.setOutputCallback(printColoredAtomic);
    shaderReloadTask.setOutputCallback(printColoredAtomic);
    materialReloadTask.setOutputCallback(printColoredAtomic);
    particleReloadTask.setOutputCallback(printColoredAtomic);
    styleProcessor.setOutputCallback(printColoredAtomic);
    hostBridgeTask.setOutputCallback(printColoredAtomic);

    std::wstring newEnv;
    if (enableIPC) {
        ipcServer->start();
        int port = ipcServer->getPort();
        printColoredAtomic("[MCDK] IPC Bridge listening on port " + std::to_string(port), ConsoleColor::Green);
        newEnv = createNewEnvironmentBlock(L"MCDEV_DEBUG_IPC_PORT", std::to_wstring(port));
    } else if (hostBridgeConfigured) {
        // A configured but invalid bridge is disabled, but its token must still not reach Minecraft.
        newEnv = createNewEnvironmentBlock(L"", L"");
    }
    if (!newEnv.empty()) {
        lpEnvironment = static_cast<void*>(newEnv.data());
    }

    STARTUPINFOW        si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};

    SECURITY_ATTRIBUTES sa{};
    sa.nLength              = sizeof(sa);
    sa.bInheritHandle       = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    // 创建 stdout/stderr 分开管道
    // Every launch handle is owned immediately so all intermediate failures close already-created resources.
    UniqueHandle outRead;
    UniqueHandle outWrite;
    UniqueHandle errRead;
    UniqueHandle errWrite;

    if (!CreatePipe(outRead.receive(), outWrite.receive(), &sa, 0)) {
        throw std::runtime_error("CreatePipe(stdout) failed");
    }

    if (!SetHandleInformation(outRead.get(), HANDLE_FLAG_INHERIT, 0)) {
        throw std::runtime_error("SetHandleInformation(stdout) failed");
    }

    if (!CreatePipe(errRead.receive(), errWrite.receive(), &sa, 0)) {
        throw std::runtime_error("CreatePipe(stderr) failed");
    }

    if (!SetHandleInformation(errRead.get(), HANDLE_FLAG_INHERIT, 0)) {
        throw std::runtime_error("SetHandleInformation(stderr) failed");
    }

    si.dwFlags    |= STARTF_USESTDHANDLES;
    si.hStdOutput  = outWrite.get();
    si.hStdError   = errWrite.get();

    // Do not expose MCDK's terminal as Minecraft stdin. A Mod calling input()
    // would otherwise block the game thread while waiting for terminal input.
    UniqueHandle nullInput(
        CreateFileW(
            L"NUL",
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            &sa,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        )
    );
    if (nullInput.get() == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("CreateFileW(NUL) failed");
    }
    si.hStdInput = nullInput.get();

    // Build command
    std::string cmd = "\"" + MCDevTool::Utils::pathToUtf8(exePath) + "\"";
    if (!userConfig.netease.chatExtension) {
        cmd.append(" chatExtension=false");
    }

    // 自定义config启动参数
    if (!config.empty()) {
        cmd.append(" config=\"" + std::string(config) + "\"");
    }

    // ptvsd 调试参数（官方调试器接口）
    auto        ptvsdConfig = mcdk::resolvePtvsdConfig(userConfig.ptvsdDebugger);
    std::string ptvsdArgs   = mcdk::buildPtvsdLaunchArgs(ptvsdConfig);
    if (!ptvsdArgs.empty()) {
        cmd.append(" " + ptvsdArgs);
        printColoredAtomic(
            "[MCDK] ptvsd 调试已启用: " + ptvsdConfig.ip + ":" + std::to_string(ptvsdConfig.port),
            ConsoleColor::Cyan
        );
    }

    auto cmdUtf16 = convertUtf8ToUtf16(cmd);
    if (!CreateProcessW(
            nullptr,
            cmdUtf16.data(),
            nullptr,
            nullptr,
            TRUE, // 继承句柄
            (lpEnvironment != nullptr ? CREATE_UNICODE_ENVIRONMENT : 0),
            lpEnvironment,
            nullptr,
            &si,
            &pi
        )) {
        throw std::runtime_error("CreateProcessW failed");
    }

    UniqueHandle processHandle(pi.hProcess);
    UniqueHandle primaryThreadHandle(pi.hThread);
    nullInput.reset();

    DWORD pid = pi.dwProcessId;
    // 设置样式处理器PID
    styleProcessor.setPid(pid);
    mcpServer.setMinecraftProcessId(pid);

    if (hostBridgeTask.enabled()) {
        hostBridgeTask.setGameStateProvider([ipcServer, debugCapabilityEnabled] {
            return mcdk::HostBridgeGameState{
                .debugCapabilityEnabled = debugCapabilityEnabled,
                .gameIpcClientCount      = ipcServer->getClientCount(),
            };
        });
        hostBridgeTask.setSessionInfo({
            .mcdkPid                = GetCurrentProcessId(),
            .minecraftPid           = pid,
            .gameIpcPort            = enableIPC ? ipcServer->getPort() : std::uint16_t{0},
            .debugCapabilityEnabled = debugCapabilityEnabled,
            .projectRoot            = std::filesystem::current_path(),
            .worldName              = userConfig.world.name,
            .worldFolderName        = userConfig.world.folderName,
            .worldRuntimePath       = MCDevTool::getMinecraftWorldsPath()
                                    / std::filesystem::u8path(userConfig.world.folderName),
            .worldSourcePath        = mcdk::resolveWorldSourcePath(userConfig.world.source),
        });
    }
    hostBridgeTask.start();

    // 父进程不需要写端
    outWrite.reset();
    errWrite.reset();

    // 输出处理回调
    auto processStdout = [needLogBuffer, logBuffer](std::string line) {
        // 屏蔽 Engine 噪音行
        if (line.find(" [INFO][Engine] ") != std::string::npos) {
            return;
        }
        // 特殊标记行处理
        if (line.find("[INFO][Developer]") != std::string::npos) {
            printColoredAtomic(line, ConsoleColor::DarkGray);
            return;
        } else if (mcdk::containsIgnoreCase(line, "SUC")) {
            printColoredAtomic(line, ConsoleColor::Green);
            return;
        } else if (mcdk::containsIgnoreCase(line, "ERROR")) {
            printColoredAtomic(line, ConsoleColor::Red);
            return;
        } else if (mcdk::containsIgnoreCase(line, "WARN")) {
            printColoredAtomic(line, ConsoleColor::Yellow);
            return;
        } else if (mcdk::containsIgnoreCase(line, "DEBUG")) {
            printColoredAtomic(line, ConsoleColor::Cyan);
            return;
        }
        printColoredAtomic(line, ConsoleColor::Default);
        if (needLogBuffer) {
            // The callback owns the line, so transfer it into the log buffer without an extra string copy.
            logBuffer->add(std::move(line));
        }
    };

    // stderr 处理回调
    auto processStderr = [needLogBuffer, logBuffer, errBuffer](std::string line) {
        static std::regex fileRe(R"(File \"([A-Za-z0-9_\.]+)\", line (\d+))");

        std::string out;
        out.reserve(line.size());

        std::sregex_iterator cur(line.begin(), line.end(), fileRe);
        std::sregex_iterator end;

        size_t lastPos = 0;

        for (; cur != end; ++cur) {
            const std::smatch& m = *cur;

            // 追加前面的普通内容
            out.append(line, lastPos, m.position() - lastPos);

            // 动态构造替换内容
            std::string dotted  = m[1].str();
            std::string slashed = dotted;
            std::replace(slashed.begin(), slashed.end(), '.', '/');
            slashed += ".py";

            out += "File \"" + slashed + "\", line " + m[2].str();

            lastPos = m.position() + m.length();
        }

        // 拼接剩余部分
        out.append(line, lastPos);

        printColoredAtomic(out, ConsoleColor::Red);
        if (needLogBuffer) {
            logBuffer->add(out);
            errBuffer->add(std::move(out));
        }
    };

    // ===================== 用户配置后置处理 =====================
    // 是否过滤非Python输出
    bool filterPython = userConfig.includeDebugMod;
    // 调试器端口（0为不启用）
    int debuggerPort = mcdk::getEnvDebuggerPort();
    if (debuggerPort == 0) {
        if (userConfig.modPcDebugger.enabled) {
            debuggerPort = userConfig.modPcDebugger.port;
        }
    }

    // 启动两个线程并行读取（避免任何死锁）
    PipeReaderThreads pipeReaders;
    pipeReaders.start(outRead.get(), errRead.get(), filterPython, processStdout, processStderr);

    if (debuggerPort > 0) {
        // 尝试启动mcdbg调试器附加（在官方调试器之前的历史产物）
        debuggerAttachToProcess(pid, debuggerPort);
    }

    if (enableAnyHotReload && modDirList != nullptr) {
        std::cout << "[HotReload] Watchers\n";
        if (modDirList) {
            for (const auto& modDirConfig : *modDirList) {
                if (modDirConfig.hotReload) {
                    std::cout << "  Mods     " << modDirConfig.getAbsoluteU8String() << "\n";
                }
            }
        }

        if (enablePyHotReload) {
            pyReloadTask.setProcessId(pid);
            pyReloadTask.setModDirs(std::vector<std::filesystem::path>(hotReloadDirs));
            pyReloadTask.start();
        }

        if (enableUiHotReload && !hotReloadUiDirs.empty()) {
            for (const auto& uiDir : hotReloadUiDirs) {
                std::cout << "  JsonUi   " << MCDevTool::Utils::pathToGenericUtf8(uiDir) << "\n";
            }
            uiReloadTask.setProcessId(pid);
            uiReloadTask.setModDirs(std::move(hotReloadUiDirs));
            uiReloadTask.start();
        }

        if (enableShaderHotReload && !hotReloadShaderDirs.empty()) {
            for (const auto& shaderDir : hotReloadShaderDirs) {
                std::cout << "  Shaders  " << MCDevTool::Utils::pathToGenericUtf8(shaderDir) << "\n";
            }
            shaderReloadTask.setProcessId(pid);
            shaderReloadTask.setModDirs(std::move(hotReloadShaderDirs));
            shaderReloadTask.start();
        }

        if (enableMaterialHotReload && !hotReloadMaterialDirs.empty()) {
            for (const auto& materialDir : hotReloadMaterialDirs) {
                std::cout << "  Material " << MCDevTool::Utils::pathToGenericUtf8(materialDir) << "\n";
            }
            materialReloadTask.setProcessId(pid);
            materialReloadTask.setModDirs(std::move(hotReloadMaterialDirs));
            materialReloadTask.start();
        }

        if (enableParticleHotReload && !hotReloadParticleDirs.empty()) {
            for (const auto& particleDir : hotReloadParticleDirs) {
                std::cout << "  Particle " << MCDevTool::Utils::pathToGenericUtf8(particleDir) << "\n";
            }
            particleReloadTask.setProcessId(pid);
            particleReloadTask.setModDirs(std::move(hotReloadParticleDirs));
            particleReloadTask.start();
        }
    }
    styleProcessor.start();

    // 等待子进程退出（子进程退出后会关闭写端，使 ReadFile 返回
    // ERROR_BROKEN_PIPE）
    WaitForSingleObject(processHandle.get(), INFINITE);

    DWORD minecraftExitCode = 0;
    if (!GetExitCodeProcess(processHandle.get(), &minecraftExitCode)) {
        minecraftExitCode = static_cast<DWORD>(-1);
    }
    hostBridgeTask.notifyMinecraftExited(minecraftExitCode);

    // 停止热更新任务
    pyReloadTask.safeExit();
    uiReloadTask.safeExit();
    shaderReloadTask.safeExit();
    materialReloadTask.safeExit();
    particleReloadTask.safeExit();
    // 停止IPC服务器 如果已启用
    ipcServer->safeExit();
    hostBridgeTask.safeExit();
    // 停止样式处理器
    styleProcessor.safeExit();
    // 安全的关闭MCP服务器(如果已启用)
    mcpServer.stop();

    // 等待读线程退出并关闭读端句柄
    pipeReaders.join();
}

#endif
