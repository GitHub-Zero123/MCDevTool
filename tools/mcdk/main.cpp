// MCDK
#include <algorithm>
#include <sstream>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
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
#include "modules/config.hpp"
#include "modules/console.hpp"
#include "modules/env.hpp"
#include "modules/hotreload.hpp"
#include "modules/level.hpp"
#include "modules/mod_dir_config.hpp"
#include "modules/mod_register.hpp"
#include "modules/style_processor.hpp"
#include "modules/utils.hpp"
#include "modules/log_buffer.hpp"
#include "modules/mcp_server.hpp"
#include "platform/runtime.hpp"


// mcdevtool api
#include <mcdevtool/addon.h>
#include <mcdevtool/utils.h>
#include <mcdevtool/debug.h>
#include <mcdevtool/env.h>
#include <mcdevtool/level.h>
#include <mcdevtool/style.h>
#include <nlohmann/json.hpp>


using mcdk::ReloadWatcherTask;
using mcdk::UserModDirConfig;
using mcdk::UserStyleProcessor;
using ConsoleColor = mcdk::ConsoleColor;
using mcdk::platform::printColoredAtomic;

static std::vector<std::string> buildGameArgs(
    const std::filesystem::path& exePath,
    std::string_view             config,
    const nlohmann::json&        userConfig
) {
    auto                     neteaseConfig = userConfig.value("netease_config", nlohmann::json::object());
    std::vector<std::string> args;
    args.push_back(MCDevTool::Utils::pathToUtf8(exePath));
    if (!neteaseConfig.value("chat_extension", false)) {
        args.emplace_back("chatExtension=false");
    }
    if (!config.empty()) {
        args.push_back("config=" + std::string(config));
    }

    auto        ptvsdConfig = mcdk::getPtvsdConfigFromJson(userConfig);
    std::string ptvsdArgs   = mcdk::buildPtvsdLaunchArgs(ptvsdConfig);
    if (!ptvsdArgs.empty()) {
        args.emplace_back("debug_ip=" + ptvsdConfig.ip);
        args.emplace_back("debug_port=" + std::to_string(ptvsdConfig.port));
        printColoredAtomic(
            "[MCDK] ptvsd debug enabled: " + ptvsdConfig.ip + ":" + std::to_string(ptvsdConfig.port),
            ConsoleColor::Cyan
        );
    }
    return args;
}

static void launchGameExe(
    const std::filesystem::path&         exePath,
    std::string_view                     config     = "",
    const nlohmann::json&                userConfig = nlohmann::json::object(),
    const std::vector<UserModDirConfig>* modDirList = nullptr
) {
    bool  autoHotReload = userConfig.value("auto_hot_reload_mods", true);
    bool  enableIPC     = autoHotReload;
    bool  needLogBuffer = false;

    auto mcpServerConfig = mcdk::getMcpServerConfigFromJson(userConfig);
    auto ipcServer       = MCDevTool::Debug::createDebugServer();
    auto logBuffer       = std::make_shared<mcdk::LogBuffer>(1000, 250);
    auto errBuffer       = std::make_shared<mcdk::LogBuffer>(1000, 400);
    auto mcpServer       = mcdk::MCPServer(mcpServerConfig);
    if (mcpServerConfig.enabled) {
        // 若启用MCP服务器将自动启用IPC调试功能
        autoHotReload = true;
        enableIPC     = true;
        needLogBuffer = true;
        printColoredAtomic(
            "[MCDK] MCP服务器已启用：" + mcpServerConfig.serverIp + ":" + std::to_string(mcpServerConfig.serverPort),
            ConsoleColor::Green
        );
        mcpServer.start();
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

                nlohmann::json params = {
                    {"code", code},
                    {"is_client", isClient}
                };
                auto result = ipcServer->requestJson("execute_code", params.dump(), 10000);
                if (!result.success) {
                    return makeTextResult(true, "Code execution failed: " + result.errorMessage);
                }

                auto response = nlohmann::json::parse(result.responseJson, nullptr, false);
                if (response.is_discarded() || !response.is_object()) {
                    return makeTextResult(true, "Code execution returned invalid JSON: " + result.responseJson);
                }
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
                    payload = response["result"];
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

        // 重载游戏
        mcpServer.setReloadGameHandler([ipcServer]() -> bool {
            if (ipcServer->getClientCount() == 0) {
                return false; // 没有连接的客户端，无法执行
            }
            return ipcServer->sendMessage(5); // GAME RELOAD
        });

        // 重载插件和游戏
        mcpServer.setReloadAddonAndGameHandler([ipcServer]() -> bool {
            if (ipcServer->getClientCount() == 0) {
                return false; // 没有连接的客户端，无法执行
            }
            return ipcServer->sendMessage(8); // ADDON AND GAME RELOAD
        });

        // 重载着色器（重新编译着色器）
        mcpServer.setReloadShadersHandler([ipcServer]() -> bool {
            if (ipcServer->getClientCount() == 0) {
                return false; // 没有连接的客户端，无法执行
            }
            return ipcServer->sendMessage(6); // SHADERS RELOAD
        });

        // 重载单个着色器（重新编译单个着色器）
        mcpServer.setReloadOnceShadersHandler([ipcServer](const std::string& fileName) -> bool {
            if (ipcServer->getClientCount() == 0) {
                return false; // 没有连接的客户端，无法执行
            }
            return ipcServer->sendMessage(7, fileName); // ONCE SHADER RELOAD
        });
    }
    mcdk::ReloadWatcherTask  reloadTask;
    mcdk::UserStyleProcessor styleProcessor(0, userConfig);
    reloadTask.setHotReloadAction([ipcServer](const nlohmann::json& targetPaths) {
        ipcServer->sendMessage(2, targetPaths.dump()); // FAST RELOAD
    });
    // reloadTask.bindServer(ipcServer);
    reloadTask.setOutputCallback(printColoredAtomic);
    styleProcessor.setOutputCallback(printColoredAtomic);

    mcdk::platform::Process             process;
    mcdk::platform::ProcessEnvironment  newEnv;
    const mcdk::platform::ProcessEnvironment* environmentBlock = nullptr;
    if (enableIPC) {
        ipcServer->start();
        int port = ipcServer->getPort();
        printColoredAtomic("[MCDK] IPC debug server started on port: " + std::to_string(port), ConsoleColor::Green);
        newEnv           = mcdk::platform::buildEnvironmentWithOverride("MCDEV_DEBUG_IPC_PORT", std::to_string(port));
        environmentBlock = &newEnv;
    }

    auto gameArgs = buildGameArgs(exePath, config, userConfig);
    std::string launchError;
    if (!mcdk::platform::startProcess(process, gameArgs, environmentBlock, launchError)) {
        mcdk::platform::closeProcess(process);
        throw std::runtime_error(launchError);
    }

    int pid = mcdk::platform::getProcessId(process);

    styleProcessor.setPid(static_cast<int>(pid));
    mcpServer.setMinecraftProcessId(static_cast<int>(pid));

    auto processStdout = [needLogBuffer, logBuffer](const std::string& line) {
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
            logBuffer->add(std::move(line));
        }
    };

    // stderr 处理回调
    auto processStderr = [needLogBuffer, logBuffer, errBuffer](const std::string& line) {
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
    bool filterPython = userConfig.value("include_debug_mod", true);
    // 调试器端口（0为不启用）
    int debuggerPort = mcdk::getEnvDebuggerPort();
    if (debuggerPort == 0) {
        // 解析用户配置覆盖
        auto debuggerConfig = userConfig.value("modpc_debugger", nlohmann::json::object());
        if (debuggerConfig.is_object()) {
            bool debuggerEnabled = debuggerConfig.value("enabled", false);
            if (debuggerEnabled) {
                debuggerPort = debuggerConfig.value("port", 5632);
            }
        }
    }

    // 启动两个线程并行读取（避免任何死锁）
    std::thread tOut = mcdk::platform::startPipeReader(
        process,
        mcdk::platform::PipeKind::Stdout,
        filterPython,
        std::function<void(const std::string&)>(processStdout)
    );

    std::thread tErr = mcdk::platform::startPipeReader(
        process,
        mcdk::platform::PipeKind::Stderr,
        filterPython,
        std::function<void(const std::string&)>(processStderr)
    );

    if (debuggerPort > 0) {
        // 尝试启动mcdbg调试器附加（在官方调试器之前的历史产物）
        mcdk::platform::attachDebuggerToProcess(pid, debuggerPort);
    }

    if (autoHotReload && modDirList != nullptr) {
        // 启动热更新追踪任务
        reloadTask.setProcessId(static_cast<int>(pid));
        // 输出追踪目录列表
        std::cout << "[HotReload] 追踪目录列表：\n";
        for (const auto& modDirConfig : *modDirList) {
            if (modDirConfig.hotReload) {
                std::cout << "  └── " << modDirConfig.getAbsoluteU8String() << "\n";
            }
        }
        reloadTask.setModDirs(UserModDirConfig::toPathList(*modDirList));
        reloadTask.start();
    }
    styleProcessor.start();

    // 等待子进程退出（子进程退出后会关闭写端，使 ReadFile 返回
    // ERROR_BROKEN_PIPE）
    mcdk::platform::waitProcess(process);

    // 停止热更新任务
    reloadTask.safeExit();
    // 停止IPC服务器 如果已启用
    ipcServer->safeExit();
    // 停止样式处理器
    styleProcessor.safeExit();
    // 安全的关闭MCP服务器(如果已启用)
    mcpServer.stop();

    // 等待读线程退出并关闭读端句柄
    tOut.join();
    tErr.join();

    mcdk::platform::closeProcess(process);
}

// ===================== 启动游戏逻辑 =====================

// 启动游戏
static void startGame(const nlohmann::json& config) {
    auto gameExePath = std::filesystem::u8path(config.value("game_executable_path", ""));
    if (!std::filesystem::is_regular_file(gameExePath)) {
        // 游戏exe路径无效 重新搜索新版
        if (mcdk::updateGamePath(gameExePath)) {
            std::cout << "游戏路径无效，重新搜索：" << MCDevTool::Utils::pathToGenericUtf8(gameExePath) << "\n";
            std::string u8input;
            std::cout << "是否更新配置文件中的游戏路径？(y/n)：";
            std::getline(std::cin, u8input);
            if (u8input == "Y" || u8input == "y") {
                mcdk::tryUpdateUserGamePath(gameExePath);
                std::cout << "已更新配置文件中的游戏路径。\n";
            } else {
                throw std::runtime_error("未更新配置文件中的游戏路径，启动终止。");
            }
        } else {
            throw std::runtime_error("未能找到有效的游戏exe文件。");
        }
    }

    auto _isSubprocessMode = mcdk::getEnvIsSubprocessMode();

    if (!_isSubprocessMode) {
        MCDevTool::cleanRuntimePacks();
    }

    auto modDirConfigs =
        UserModDirConfig::parseListFromJson(config.value("included_mod_dirs", nlohmann::json::array({"./"})));

    if (_isSubprocessMode) {
        // 子进程模式 直接启动游戏exe（通常由vsc插件多开使用）
        launchGameExe(gameExePath, "", config, &modDirConfigs);
        return;
    }
    std::vector<MCDevTool::Addon::PackInfo> linkedPacks;
    if (config.value("include_debug_mod", true)) {
        auto debugMod = mcdk::registerDebugMod(config, modDirConfigs);
        std::cout << "[MCDK] 已注册调试MOD：" << debugMod.uuid << "\n";
        linkedPacks.push_back(std::move(debugMod));
        mcdk::linkUserConfigModDirs(modDirConfigs, linkedPacks);
    } else {
        mcdk::linkUserConfigModDirs(modDirConfigs, linkedPacks);
    }

    // 创建世界
    auto worldFolderName = config.value("world_folder_name", "MC_DEV_WORLD");
    auto resetWorld      = config.value("reset_world", false); // 若启用该参数 每次都会强制覆盖世界
    auto worldsPath      = MCDevTool::getMinecraftWorldsPath() / std::filesystem::u8path(worldFolderName);
    if (!std::filesystem::is_directory(worldsPath) || resetWorld) {
        std::filesystem::remove_all(worldsPath);
        if (resetWorld) {
            std::cout << "已删除旧世界数据，正在创建新世界...\n";
        }
        std::filesystem::create_directories(worldsPath);
        std::ofstream levelFile(worldsPath / "level.dat", std::ios::binary);
        auto          levelDat = mcdk::createUserLevel(config);
        levelFile.write(reinterpret_cast<const char*>(levelDat.data()), levelDat.size());
        levelFile.close();
    } else {
        // 更新level.dat的配置数据
        if (mcdk::getEnvIsPluginEnv()) {
            // 插件环境每次启动都要覆盖配置
            auto levelOptions = mcdk::parseLevelOptionsFromUserConfig(config);
            MCDevTool::Level::updateLevelDatWorldDataInFile(worldsPath / "level.dat", std::nullopt, levelOptions);
        } else {
            // 非插件环境只更新时间戳
            MCDevTool::Level::updateLevelDatLastPlayedInFile(worldsPath / "level.dat");
        }
    }

    // netease_world_behavior_packs.json / netease_world_resource_packs.json
    auto behPacksManifest = nlohmann::json::array();
    auto resPacksManifest = nlohmann::json::array();
    for (const auto& pack : linkedPacks) {
        nlohmann::json packEntry{
            {"pack_id", pack.uuid},
            {"version", nlohmann::json::parse(pack.version)},
        };
        if (pack.type == MCDevTool::Addon::PackType::BEHAVIOR) {
            behPacksManifest.push_back(std::move(packEntry));
        } else if (pack.type == MCDevTool::Addon::PackType::RESOURCE) {
            resPacksManifest.push_back(std::move(packEntry));
        }
    }

    auto autoJoinGame = config.value("auto_join_game", true);
    auto envAutoJoin  = mcdk::getEnvAutoJoinGameState();
    if (envAutoJoin != -1) {
        // 环境变量覆写配置文件
        autoJoinGame = (envAutoJoin == 1);
    }
    std::string targetBehJson = "netease_world_behavior_packs.json";
    std::string targetResJson = "netease_world_resource_packs.json";
    if (autoJoinGame) {
        // 使用国际版标准协议 避免网易串改
        targetBehJson = "world_behavior_packs.json";
        targetResJson = "world_resource_packs.json";
    }
    std::ofstream behManifestFile(worldsPath / targetBehJson);
    behManifestFile << behPacksManifest.dump(4);
    behManifestFile.close();

    std::ofstream resManifestFile(worldsPath / targetResJson);
    resManifestFile << resPacksManifest.dump(4);
    resManifestFile.close();

    if (!autoJoinGame) {
        // 不自动进入游戏 直接启动游戏exe
        launchGameExe(gameExePath, "", config, &modDirConfigs);
        return;
    }

    auto configPath = worldsPath / "dev_config.cppconfig";
    // 创建dev_config
    nlohmann::json devConfig{
        {"world_info", {{"level_id", worldFolderName}}},
        {"room_info", nlohmann::json::object()},
        {"player_info",
         {
             {"urs", ""},
             {"user_id", 0},
             {"user_name", config.value("user_name", "developer")},
         }},
    };

    auto defaultSkinPath =
        MCDevTool::Utils::pathToGenericUtf8(gameExePath.parent_path() / "data/skin_packs/vanilla/steve.png");

    if (config.contains("skin_info") && config["skin_info"].is_object()) {
        // 用户自定义skin_info
        devConfig["skin_info"] = config["skin_info"];
        auto& skinInfo         = devConfig["skin_info"];
        // 自动生成缺失字段
        if (!skinInfo.contains("slim")) {
            skinInfo["slim"] = false;
        }
        std::string skinPath = skinInfo.value("skin", "");
        if (skinPath.empty()) {
            skinInfo["skin"] = defaultSkinPath;
            skinPath         = std::move(defaultSkinPath);
        }
        // 安全校验 检查文件是否存在
        if (!skinPath.empty()) {
            auto fSkinPath = std::filesystem::u8path(skinPath);
            if (!std::filesystem::is_regular_file(fSkinPath)) {
                throw std::runtime_error("自定义皮肤文件不存在：" + skinPath);
            }
        }
    } else {
        // 自动生成skin_info
        devConfig["skin_info"] = {{"slim", false}, {"skin", std::move(defaultSkinPath)}};
    }
    std::ofstream configFile(configPath);
    configFile << devConfig.dump(4);
    configFile.close();
    launchGameExe(gameExePath, MCDevTool::Utils::pathToGenericUtf8(configPath), config, &modDirConfigs);
}

#ifdef MCDK_ENABLE_CLI
#ifdef _WIN32
int MCDK_CLI_PARSE(int argc, wchar_t* argv[]);
#else
int MCDK_CLI_PARSE(int argc, char* argv[]);
#endif
#endif

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
#else
int main(int argc, char* argv[]) {
#endif
    mcdk::platform::setupConsole();

    if (mcdk::getEnvOutputMode() == 1) {
        setvbuf(stdout, nullptr, _IONBF, 0);
    }

#ifdef NDEBUG
    try {
#endif
#ifdef MCDK_ENABLE_CLI
        if (argc > 1) {
            return MCDK_CLI_PARSE(argc, argv);
        }
#endif
        auto config = mcdk::userParseConfig();
        startGame(config);
#ifdef NDEBUG
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n";
        return 1;
    }
#endif
    return 0;
}
