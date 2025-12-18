#include <iostream>
#include <string>
#include <string_view>
#include <fstream>
#include <iterator>
#include <filesystem>
#include <regex>
// std::move
#include <utility>
#include <vector>
#include <thread>
#include <mutex>
#include <functional>
#include <unordered_set>
#include <optional>
#include <stdexcept>
#include <cstdint>
#include <algorithm>
#include <INCLUDE_MOD.h>
#include <mcdevtool/env.h>
#include <mcdevtool/addon.h>
#include <mcdevtool/level.h>
#include <mcdevtool/debug.h>
#include <mcdevtool/style.h>
#include <nlohmann/json.hpp>

// #define MCDEV_EXPERIMENTAL_LAUNCH_WITH_CONFIG_PATH

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
#endif

// 获取环境变量MCDEV_OUTPUT_MODE的输出策略：0.默认，视为终端处理  1.视为vscode/pycharm的调试伪终端处理
static int _GET_ENV_MCDEV_OUTPUT_MODE() {
    static int outputMode = -1;
    if(outputMode != -1) {
        return outputMode;
    }
    auto* modeStr = std::getenv("MCDEV_OUTPUT_MODE");
    if(modeStr == nullptr) {
        outputMode = 0;
    } else {
        try {
            outputMode = std::stoi(modeStr);
        } catch(...) {
            outputMode = 0;
        }
    }
    return outputMode;
}

// 获取环境变量 强制修改调试器端口号
static int _GET_ENV_DEBUGGER_PORT() {
    auto* portStr = std::getenv("MCDEV_MODPC_DEBUGGER_PORT");
    if(portStr == nullptr) {
        return 0;
    }
    try {
        int port = std::stoi(portStr);
        if(port > 0 && port <= 65535) {
            return port;
        }
    } catch(...) {}
    return 0;
}

// 获取环境变量 是否是子进程模式
static bool _GET_ENV_IS_SUBPROCESS_MODE() {
    auto* valStr = std::getenv("MCDEV_IS_SUBPROCESS_MODE");
    if(valStr == nullptr) {
        return false;
    }
    std::string val(valStr);
    std::transform(val.begin(), val.end(), val.begin(), ::tolower);
    return (val == "1" || val == "true" || val == "yes");
}

// 字符串关键字替换
static void stringReplace(std::string& str, const std::string& from, const std::string& to) {
    size_t startPos = 0;
    while((startPos = str.find(from, startPos)) != std::string::npos) {
        str.replace(startPos, from.length(), to);
        startPos += to.length();
    }
}

// 用户mod目录配置结构体
class UserModDirConfig {
public:
    std::filesystem::path path;
    bool hotReload = false;

    UserModDirConfig() = default;
    explicit UserModDirConfig(const std::filesystem::path& p, bool hr)
        : path(p), hotReload(hr) {}

    // 获取基于工作区的绝对路径
    std::filesystem::path getAbsolutePath() const {
        static const auto workingDir = std::filesystem::current_path();
        if(path.is_absolute()) {
            return path.lexically_normal();
        } else {
            return (workingDir / path).lexically_normal();
        }
    }

    // 获取绝对路径的utf8 linux风格字符串
    std::string getAbsoluteU8String() const {
        auto absPath = getAbsolutePath().generic_u8string();
        if constexpr (sizeof(char8_t) == sizeof(char)) {
            return reinterpret_cast<const char*>(absPath.c_str());
        } else {
            return std::string(absPath.begin(), absPath.end());
        }
    }

    // 从json对象解析UserModDirConfig
    static UserModDirConfig parseFromJson(const nlohmann::json& j) {
        UserModDirConfig config;
        if(j.is_string()) {
            config.path = std::filesystem::u8path(j.get<std::string>());
            config.hotReload = true;
        } else if(j.is_object()) {
            config.path = std::filesystem::u8path(j.value("path", "./"));
            config.hotReload = j.value("hot_reload", true);
        } else {
            throw std::runtime_error("Invalid mod directory configuration format.");
        }
        return config;
    }

    // 从json数组解析UserModDirConfig列表
    static std::vector<UserModDirConfig> parseListFromJson(const nlohmann::json& jArray) {
        std::vector<UserModDirConfig> configs;
        if(!jArray.is_array()) {
            throw std::runtime_error("Mod directories configuration should be an array.");
        }
        for(const auto& item : jArray) {
            configs.push_back(parseFromJson(item));
        }
        return configs;
    }

    // 基于std::vector<std::string>构造UserModDirConfig列表
    static std::vector<UserModDirConfig> fromStringList(const std::vector<std::string>& u8Paths) {
        std::vector<UserModDirConfig> configs;
        for(const auto& u8Path : u8Paths) {
            configs.emplace_back(std::filesystem::u8path(u8Path), true);
        }
        return configs;
    }

    // 将一组vector<UserModDirConfig>生成字符串形式的hot_reload追踪列表（json数组格式）
    static std::string toHotReloadListString(const std::vector<UserModDirConfig>& configs) {
        nlohmann::json jArray = nlohmann::json::array();
        for(const auto& config : configs) {
            if(config.hotReload) {
                jArray.push_back(config.getAbsoluteU8String());
            }
        }
        return jArray.dump();
    }

    // 将std::vector<UserModDirConfig>转换为std::vector<std::filesystem::path>的目标追踪列表
    static std::vector<std::filesystem::path> toPathList(const std::vector<UserModDirConfig>& configs) {
        std::vector<std::filesystem::path> paths;
        for(const auto& config : configs) {
            if(config.hotReload) {
                paths.push_back(config.getAbsolutePath());
            }
        }
        return paths;
    }
};

static nlohmann::json createDefaultConfig() {
    std::filesystem::path exePath;
    auto autoExePath = MCDevTool::autoMatchLatestGameExePath();
    if(!autoExePath.has_value()) {
        // 无法自动匹配到游戏exe路径，要求用户输入
        std::string u8input;
        std::cout << "请输入游戏可执行文件路径：";
        std::getline(std::cin, u8input);
        if(u8input.size() > 2 && u8input[0] == '"' && u8input[u8input.size() - 1] == '"') {
            // 针对字符串形式路径解析
            u8input = u8input.substr(1, u8input.size() - 2);
        }
        exePath = std::filesystem::u8path(u8input);
    } else {
        exePath = autoExePath.value();
    }
    if(!std::filesystem::is_regular_file(exePath)) {
        std::cerr << "路径无效，文件不存在。\n";
        exit(1);
    }
    nlohmann::json config {
        // 包含的mod目录，支持多个
        { "included_mod_dirs", nlohmann::json::array({"./"}) },
        // 世界随机种子 留空则随机生成
        { "world_seed", nullptr },
        // 是否重置世界
        { "reset_world", false },
        // 世界名称
        { "world_name", "MC_DEV_WORLD" },
        // 世界文件夹名称
        { "world_folder_name", "MC_DEV_WORLD" },
        // 是否自动进入游戏存档
        { "auto_join_game", true },
        // 包含调试模组(提供R键热更新以及py输出流标记)
        { "include_debug_mod", true },
        // 是否启用自动热更新mod功能
        { "auto_hot_reload_mods", true },
        // 世界类型 0-旧版 1-无限 2-平坦
        { "world_type", 1 },
        // 游戏模式 0-生存 1-创造 2-冒险
        { "game_mode", 1 },
        // 是否启用作弊
        { "enable_cheats", true },
        // 保留物品栏
        { "keep_inventory", true },
        // { "modpc_debugger", {
        //     // 默认不启用调试器附加
        //     { "enabled", false },
        //     { "port", 5632 }
        // } },
        // user_name可选参数
        // { "user_name", "developer" },
        // skin info(可选参数)
        // { "skin_info", nlohmann::json::object() },
        // 自定义debug参数
        // { "debug_options", nlohmann::json::object() },
    };
    // 游戏可执行文件路径
    auto u8Path = exePath.generic_u8string();
    if constexpr (sizeof(char8_t) == sizeof(char)) {
        config["game_executable_path"] = reinterpret_cast<const char*>(u8Path.c_str());
    } else {
        config["game_executable_path"] = std::string(u8Path.begin(), u8Path.end());
    }
    return config;
}

// 解析用户配置文件
static nlohmann::json userParseConfig() {
    nlohmann::json config;
    auto configPath = std::filesystem::current_path() / ".mcdev.json";
    if(!std::filesystem::is_regular_file(configPath)) {
        // 初始化默认配置文件
        config = createDefaultConfig();
        std::ofstream configFile(configPath);
        configFile << config.dump(4);
        configFile.close();
    } else {
        std::ifstream configFile(configPath, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(configFile)),
                             std::istreambuf_iterator<char>());
        // 启用注释支持
        config = nlohmann::json::parse(content, nullptr, false, true);
        configFile.close();
        if(config.is_discarded()) {
            throw std::runtime_error("配置文件解析失败，JSON异常，请检查格式是否正确。");
        }
    }
    return config;
}

// 注册调试MOD
MCDevTool::Addon::PackInfo registerDebugMod(const nlohmann::json& config,
                    const std::vector<UserModDirConfig>& modDirConfigs, std::filesystem::path* outConfigFile = nullptr) {
    auto manifest = INCLUDE_MOD_RES::resourceMap.at("manifest.json");
    std::string manifestContent(reinterpret_cast<const char*>(manifest.first), manifest.second);
    MCDevTool::Addon::PackInfo info;
    parseJsonPackInfo(manifestContent, info);
    std::filesystem::path outDir;

    if(info.type == MCDevTool::Addon::PackType::BEHAVIOR) {
        outDir = MCDevTool::getBehaviorPacksPath();
    } else if(info.type == MCDevTool::Addon::PackType::RESOURCE) {
        outDir = MCDevTool::getResourcePacksPath();
    } else {
        throw std::runtime_error("调试MOD的PackType类型未知，无法注册。");
    }

    std::string uuidNoDash = info.uuid;
    uuidNoDash.erase(std::remove(uuidNoDash.begin(), uuidNoDash.end(), '-'), uuidNoDash.end());
    auto target = outDir / uuidNoDash;

    // 写入ADDON数据到目标目录
    if(std::filesystem::exists(target)) {
        std::filesystem::remove_all(target);
    }

    // 处理debug_options参数
    auto configDebugOptions = config.value("debug_options", nlohmann::json::object());

    // // 是否启用自动热更新py文件
    // bool AUTO_RELOAD_PY_FILES = config.value("auto_hot_reload_mods", false);

    // // 是否创建IPC链接端口
    // if(AUTO_RELOAD_PY_FILES) {
    //     std::cout << "[auto_reload_py_files] 已启用自动热更检测，正在创建跟踪线程...\n";
    //     int IPC_PORT = 0;
    //     configDebugOptions["ipc_port"] = IPC_PORT;
    // }

    // 生成格式化的字面量json
    auto DEBUG_OPTIONS = configDebugOptions.dump();

    // 替换为python boolean格式
    stringReplace(DEBUG_OPTIONS, "true", "True");
    stringReplace(DEBUG_OPTIONS, "false", "False");
    stringReplace(DEBUG_OPTIONS, "null", "None");

    for(const auto& [resName, resData] : INCLUDE_MOD_RES::resourceMap) {
        auto resPath = target / std::filesystem::u8path(resName);
        std::filesystem::create_directories(resPath.parent_path());
        std::ofstream resFile(resPath, std::ios::binary);
        if(resName.ends_with("Config.py")) {
            // 替换关键字实现传参
            std::string content(reinterpret_cast<const char*>(resData.first), resData.second);
            stringReplace(content, "\"{#debug_options}\"", DEBUG_OPTIONS);
            stringReplace(content, "\"{#target_mod_dirs}\"", UserModDirConfig::toHotReloadListString(modDirConfigs));
            resFile.write(content.data(), content.size());
            if(outConfigFile != nullptr) {
                outConfigFile->assign(resPath);
            }
        } else {
            resFile.write(reinterpret_cast<const char*>(resData.first), resData.second);
        }
        resFile.close();
    }

    return info;
}

// link用户mod目录
static void linkUserMods(const std::vector<std::string>& u8Paths, std::vector<MCDevTool::Addon::PackInfo>& linkedPacks) {
    for(const auto& targetDir : u8Paths) {
        auto dir = std::filesystem::u8path(targetDir);
        auto packInfos = MCDevTool::linkSourceAddonToRuntimePacks(dir);
        for(const auto& info : packInfos) {
            if(info.type == MCDevTool::Addon::PackType::BEHAVIOR) {
                std::cout << "LINK行为包: \"" << info.name << "\", UUID: " << info.uuid << "\n";
            } else if(info.type == MCDevTool::Addon::PackType::RESOURCE) {
                std::cout << "LINK资源包: \"" << info.name << "\", UUID: " << info.uuid << "\n";
            }
            linkedPacks.push_back(std::move(info));
        }
    }
}

// link用户mod目录 基于配置结构体
static void linkUserConfigModDirs(std::vector<UserModDirConfig>& configs,
        std::vector<MCDevTool::Addon::PackInfo>& linkedPacks, bool updateConfigPaths = false) {
    for(auto& modConfig : configs) {
        auto dir = modConfig.getAbsolutePath();
        auto packInfos = MCDevTool::linkSourceAddonToRuntimePacks(dir);
        for(const auto& info : packInfos) {
            if(info.type == MCDevTool::Addon::PackType::BEHAVIOR) {
                std::cout << "LINK行为包: \"" << info.name << "\", UUID: " << info.uuid << "\n";
                if(modConfig.hotReload) {
                    std::cout << "  -> 热更新标记追踪\n";
                    if(updateConfigPaths) {
                        modConfig.path = info.path;   // 重新更新为link后的路径
                    }
                }
            } else if(info.type == MCDevTool::Addon::PackType::RESOURCE) {
                std::cout << "LINK资源包: \"" << info.name << "\", UUID: " << info.uuid << "\n";
            }
            linkedPacks.push_back(std::move(info));
        }
    }
}

// 基于用户config生成LevelOptions数据
static MCDevTool::Level::LevelOptions parseLevelOptionsFromUserConfig(const nlohmann::json& config) {
    MCDevTool::Level::LevelOptions options;
    options.worldType = static_cast<uint32_t>(config.value("world_type", 1));
    options.gameMode = static_cast<uint32_t>(config.value("game_mode", 1));
    if(config.contains("world_seed") && !config["world_seed"].is_null()) {
        options.seed = config["world_seed"].get<uint64_t>();
    }
    options.enableCheats = config.value("enable_cheats", true);
    options.keepInventory = config.value("keep_inventory", true);
    options.doWeatherCycle = config.value("do_weather_cycle", true);
    // 处理实验性选项
    MCDevTool::Level::ExperimentsOptions expOptions;
    // 检查存在experiment_options字段
    if(config.contains("experiment_options")) {
        auto experimentOptions = config["experiment_options"];
        // 实验性玩法参数处理
        if(experimentOptions.is_object()) {
            expOptions.enable = true;
            expOptions.dataDrivenBiomes = experimentOptions.value("data_driven_biomes", false);
            expOptions.dataDrivenItems = experimentOptions.value("data_driven_items", false);
            expOptions.experimentalMolangFeatures = experimentOptions.value("experimental_molang_features", false);
            options.experimentsOptions = expOptions;
        }
    }
    return options;
}

// 根据用户config创建level.dat
static std::vector<uint8_t> createUserLevel(const nlohmann::json& config) {
    auto options = parseLevelOptionsFromUserConfig(config);
    std::string worldName = config.value("world_name", "MC_DEV_WORLD");
    // std::string folderName = config.value("world_folder_name", "MC_DEV_WORLD");
    auto levelDat = MCDevTool::Level::createDefaultLevelDat(worldName, options);
    return levelDat;
}

// #ifdef MCDEV_EXPERIMENTAL_LAUNCH_WITH_CONFIG_PATH

static std::mutex g_consoleMutex;

enum class ConsoleColor {
    Default,
    Green,      // 绿色（Green）
    Red,        // 红色（Red）
    Blue,       // 蓝色（Blue）
    Yellow,    // 黄色（Yellow）
    Cyan,      // 青色（Cyan）
    Magenta,   // 品红（Magenta）
    White,     // 白色（White）
    Black,     // 黑色（Black）
    Gray,      // 亮灰（Light Gray）
    DarkGray   // 深灰（Dark Gray）
};

// 线程安全彩色输出
static void printColoredAtomic(const std::string& msg, ConsoleColor color) {
    std::lock_guard<std::mutex> lk(g_consoleMutex);
    HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);

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

    if(color == ConsoleColor::Default) {
        return;
    }
    // 恢复原色
    SetConsoleTextAttribute(hConsole, info.wAttributes);
}

// 进程buffer行处理
static void processBufferAppend(std::string& lineBuf, const char* buf, size_t len,
                                bool filterPython,
                                const std::function<void(const std::string&)>& processLine)
{
    lineBuf.append(buf, len);

    size_t pos = 0;
    while ((pos = lineBuf.find('\n')) != std::string::npos) {
        std::string line = lineBuf.substr(0, pos);
        lineBuf.erase(0, pos + 1);

        // 去除行尾可能存在的 '\r'
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        // 过滤：若启用只保留 [Python] 则丢弃其它
        if (filterPython && line.find("[Python] ") == std::string::npos)
            continue;

        processLine(line);
    }
}

#ifdef _WIN32

// pipe线程处理函数
static void readPipeThread(HANDLE hPipe, bool filterPython,
                           const std::function<void(const std::string&)>& processLine)
{
    constexpr DWORD BUFSZ = 4096;
    std::string lineBuf;
    std::vector<char> buffer(BUFSZ);

    while (true) {
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(hPipe, buffer.data(), BUFSZ, &bytesRead, NULL);
        if (!ok) {
            DWORD err = GetLastError();
            // ERROR_BROKEN_PIPE (109) 表示写端已关闭并读尽
            if (err == ERROR_BROKEN_PIPE) {
                // 处理残留并退出
                if (!lineBuf.empty()) {
                    // 没有换行但还有内容，作为最后一行处理
                    std::string lastLine = lineBuf;
                    if (!lastLine.empty() && lastLine.back() == '\r') lastLine.pop_back();
                    if (!(filterPython && lastLine.find("[Python] ") == std::string::npos))
                        processLine(lastLine);
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
                if (!(filterPython && lastLine.find("[Python] ") == std::string::npos))
                    processLine(lastLine);
                lineBuf.clear();
            }
            break;
        }

        // 追加并按行处理（会把完整行交给 processLine，残留留在 lineBuf）
        processBufferAppend(lineBuf, buffer.data(), bytesRead, filterPython, processLine);
    }
}

// 尝试附加调试器到指定进程
static void debuggerAttachToProcess(DWORD pid, int port) {
    // 执行cmd调用mcdbg.exe附加（如果失败则输出错误信息）
    std::string cmd;
    cmd.reserve(48);

    cmd.append("mcdbg.exe --pid ");
    cmd.append(std::to_string(pid));
    cmd.append(" --port ");
    cmd.append(std::to_string(port));
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(
        nullptr,
        cmd.data(),
        nullptr, nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
        &si,
        &pi
    )) {
        std::cerr << "警告：无法启动mcdbg.exe附加调试器，请确保其在环境变量路径中。" << _MCDEV_LOG_OUTPUT_ENDL;
        return;
    }
    std::cout << "调试器已启动，正在附加到进程PID：" << pid << " 端口：" << port << " ..." << _MCDEV_LOG_OUTPUT_ENDL;
}

// 不区分大小写的字符串包含检查
static bool containsIgnoreCase(std::string_view text,
                               std::string_view pattern) {
    if (pattern.empty()) {
        return true;
    }

    if (pattern.size() > text.size()) {
        return false;
    }

    for (size_t i = 0; i <= text.size() - pattern.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < pattern.size(); ++j) {
            char a = text[i + j];
            char b = pattern[j];

            // ASCII tolower（无 locale）
            if (a >= 'A' && a <= 'Z') {
                a += 'a' - 'A';
            }
            if (b >= 'A' && b <= 'Z') {
                b += 'a' - 'A';
            }

            if (a != b) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

// 检查用户配置是否需要启用IPC调试功能
static bool checkUserConfigEnableIPC(const nlohmann::json& userConfig) {
    // 若启用了auto_hot_reload_mods则启用IPC
    bool autoHotReload = userConfig.value("auto_hot_reload_mods", true);
    if(autoHotReload) {
        return true;
    }
    return false;
}

// 将utf8的string转换为utf16的wstring
static std::wstring utf8ToUtf16(const std::string& utf8Str) {
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
static std::wstring createNewEnvironmentBlock(const std::wstring& newVar, const std::wstring& newValue) {
    // 获取当前环境变量块
    LPWCH envBlock = GetEnvironmentStringsW();
    if (envBlock == nullptr) {
        throw std::runtime_error("Failed to get current environment strings.");
    }

    std::wstring newEnvBlock;
    // 复制现有环境变量
    LPWCH current = envBlock;
    while (*current) {
        std::wstring varLine(current);
        newEnvBlock += varLine + L'\0';
        current += varLine.size() + 1;
    }

    // 添加新的环境变量
    newEnvBlock += newVar + L'=' + newValue + L'\0';

    // 结束环境变量块
    newEnvBlock += L'\0';

    FreeEnvironmentStringsW(envBlock);
    return newEnvBlock;
}

// 热更新监视任务类
class ReloadWatcherTask : public MCDevTool::Debug::HotReloadWatcherTask {
public:
    using MCDevTool::Debug::HotReloadWatcherTask::HotReloadWatcherTask;

    // 从文件路径计算Python模块名
    static void pyPathToModuleName(const std::filesystem::path& filePath, std::string& outModuleName) {
        std::filesystem::path cur = filePath;
        std::filesystem::path manifestDir;

        // 向上查找 manifest.json
        while (true) {
            if (std::filesystem::exists(cur / "manifest.json")) {
                manifestDir = cur;
                break;
            }

            auto parent = cur.parent_path();
            if (parent == cur) {
                return; // 没找到 manifest
            }
            cur = parent;
        }

        // 计算 filePath 相对于 manifestDir 的路径
        std::filesystem::path rel = std::filesystem::relative(filePath, manifestDir);
        if (rel.empty()) {
            return;
        }

        std::vector<std::string> parts;
        for (const auto& p : rel) {
            parts.push_back(p.string());
        }

        if (parts.empty()) { return; }

        std::string& last = parts.back();
        if (last.size() > 3 && last.ends_with(".py")) {
            last.resize(last.size() - 3);
        }

        // 拼接模块名
        std::string moduleName;
        for (size_t i = 0; i < parts.size(); ++i) {
            moduleName += parts[i];
            if (i + 1 < parts.size()) {
                moduleName.push_back('.');
            }
        }
        outModuleName = std::move(moduleName);
    }

    void onHotReloadTriggered() override {
        // printColoredAtomic("[HotReload] 检测到修改，已触发热更新。", ConsoleColor::Yellow);
        // mIpcServer->sendMessage(1); // 发送热更新命令
        nlohmann::json targetPaths = nlohmann::json::array();
        {
            std::lock_guard<std::mutex> lock(gMutex);
            for(const auto& modulePath : mCachedPyModulePaths) {
                std::string moduleName;
                pyPathToModuleName(modulePath, moduleName);
                if(moduleName.empty()) {
                    continue;
                }
                targetPaths.push_back(std::move(moduleName));
            }
            mCachedPyModulePaths.clear();
        }
        if(targetPaths.empty()) {
            return;
        }
        printColoredAtomic("[HotReload] 检测到修改，已触发热更新。", ConsoleColor::Yellow);
        mIpcServer->sendMessage(2, targetPaths.dump()); // FAST RELOAD
    }

    void onFileChanged(const std::filesystem::path& filePath) override {
        // 输出变更文件路径
        auto u8Path = filePath.generic_u8string();
        printColoredAtomic("[HotReload] Detected change in: " + std::string(u8Path.begin(), u8Path.end()), ConsoleColor::Yellow);
        std::lock_guard<std::mutex> lock(gMutex);
        mCachedPyModulePaths.insert(filePath);
    }

    void bindServer(const std::shared_ptr<MCDevTool::Debug::DebugIPCServer>& server) {
        mIpcServer = server;
    }
private:
    std::shared_ptr<MCDevTool::Debug::DebugIPCServer> mIpcServer;
    std::unordered_set<std::filesystem::path> mCachedPyModulePaths;
    std::mutex gMutex;
};

// 用户样式处理类
class UserStyleProcessor : public MCDevTool::Style::MinecraftWindowStyler {
public:
    UserStyleProcessor(int pid, const nlohmann::json& userConfig)
        : MCDevTool::Style::MinecraftWindowStyler(pid)
    {
        // 解析用户配置
        // 检查是否存在 window_style 字段
        if(!userConfig.contains("window_style")) {
            return;
        }
        auto& styleConfig = userConfig["window_style"];
        MCDevTool::Style::StyleConfig config;
        config.alwaysOnTop = styleConfig.value("always_on_top", false);
        config.hideTitleBar = styleConfig.value("hide_title_bar", false);
        if(config.hideTitleBar || config.alwaysOnTop) {
            mNeedUpdateStyle = true;
        }
        // 处理 title_bar_color
        if(styleConfig.contains("title_bar_color") && styleConfig["title_bar_color"].is_array()
            && styleConfig["title_bar_color"].size() >= 3) {
            auto colorArray = styleConfig["title_bar_color"];
            config.titleBarColor = std::vector<uint8_t>{
                colorArray[0].get<uint8_t>(),
                colorArray[1].get<uint8_t>(),
                colorArray[2].get<uint8_t>()
            };
            mNeedUpdateStyle = true;
        }
        // 处理 fixed_size（用 int 保存，支持大分辨率）
        if(styleConfig.contains("fixed_size") && styleConfig["fixed_size"].is_array()
            && styleConfig["fixed_size"].size() >= 2) {
            auto sizeArray = styleConfig["fixed_size"];
            config.fixedSize = std::vector<int>{
                sizeArray[0].get<int>(),
                sizeArray[1].get<int>()
            };
            mNeedUpdateStyle = true;
        }
        // 处理 fixed_position（同样用 int 保存像素坐标）
        if(styleConfig.contains("fixed_position") && styleConfig["fixed_position"].is_array()
            && styleConfig["fixed_position"].size() >= 2) {
            auto posArray = styleConfig["fixed_position"];
            config.fixedPosition = std::vector<int>{
                posArray[0].get<int>(),
                posArray[1].get<int>()
            };
            mNeedUpdateStyle = true;
        }
        // 处理 lock_corner
        if(styleConfig.contains("lock_corner") && styleConfig["lock_corner"].is_number_integer()) {
            config.lockCorner = styleConfig["lock_corner"].get<int>();
            mNeedUpdateStyle = true;
        }
        mConfig = std::move(config);
    }

    void start() {
        if(mNeedUpdateStyle) {
            MinecraftWindowStyler::start();
            printColoredAtomic("[Style] 已启用自定义样式，等待更新窗口中。", ConsoleColor::Cyan);
        }
    }

    void onStyleApplied() override {
        printColoredAtomic("[Style] 窗口样式更新已应用。", ConsoleColor::Cyan);
    }
private:
    bool mNeedUpdateStyle = false;
};

// 启动游戏可执行文件
static void launchGameExe(const std::filesystem::path& exePath, std::string_view config = "",
    const nlohmann::json& userConfig = nlohmann::json::object(), const std::vector<UserModDirConfig>* modDirList=nullptr)
{
    bool autoHotReload = userConfig.value("auto_hot_reload_mods", true);
    bool enableIPC = autoHotReload;
    void* lpEnvironment = nullptr;

    auto ipcServer = MCDevTool::Debug::createDebugServer();
    ReloadWatcherTask reloadTask;
    UserStyleProcessor styleProcessor(0, userConfig);
    reloadTask.bindServer(ipcServer);

    std::wstring newEnv;
    if(enableIPC) {
        ipcServer->start();
        int port = ipcServer->getPort();
        std::cout << "IPC调试服务器已启动，端口：" << port << _MCDEV_LOG_OUTPUT_ENDL;
        newEnv = createNewEnvironmentBlock(L"MCDEV_DEBUG_IPC_PORT", std::to_wstring(port));
        lpEnvironment = (void*)newEnv.data();
    }

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    // 创建 stdout/stderr 分开管道
    HANDLE outRead = NULL, outWrite = NULL;
    HANDLE errRead = NULL, errWrite = NULL;

    if (!CreatePipe(&outRead, &outWrite, &sa, 0)) {
        throw std::runtime_error("CreatePipe(stdout) failed");
    }

    if (!SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0)) {
        throw std::runtime_error("SetHandleInformation(stdout) failed");
    }

    if (!CreatePipe(&errRead, &errWrite, &sa, 0)) {
        throw std::runtime_error("CreatePipe(stderr) failed");
    }

    if (!SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0)) {
        throw std::runtime_error("SetHandleInformation(stderr) failed");
    }

    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdOutput = outWrite;
    si.hStdError  = errWrite;
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);

    // Build command
    std::string cmd = "\"" + exePath.string() + "\"";
    if (!config.empty())
        cmd += " config=\"" + std::string(config) + "\"";
    // std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    // cmdBuf.push_back('\0');

    if (!CreateProcessW(
        nullptr,
        utf8ToUtf16(cmd).data(),
        nullptr, nullptr,
        TRUE,   // 继承句柄
        (lpEnvironment != nullptr ? CREATE_UNICODE_ENVIRONMENT : 0),
        lpEnvironment,
        nullptr,
        &si,
        &pi
    )) {
        CloseHandle(outRead); CloseHandle(outWrite);
        CloseHandle(errRead); CloseHandle(errWrite);
        throw std::runtime_error("CreateProcessA failed");
    }

    DWORD pid = pi.dwProcessId;
    // 设置样式处理器PID
    styleProcessor.setPid(pid);

    // 父进程不需要写端
    CloseHandle(outWrite);
    CloseHandle(errWrite);

    // 输出处理回调
    auto processStdout = [](const std::string& line) {
        // 屏蔽的 Engine 噪音行
        if (line.find(" [INFO][Engine] ") != std::string::npos) {
            return;
        }
        // 特殊标记行处理
        if(line.find("[INFO][Developer]") != std::string::npos) {
            printColoredAtomic(line, ConsoleColor::DarkGray);
            return;
        }
        else if(containsIgnoreCase(line, "SUC")) {
            printColoredAtomic(line, ConsoleColor::Green);
            return;
        } else if(containsIgnoreCase(line, "ERROR")) {
            printColoredAtomic(line, ConsoleColor::Red);
            return;
        } else if(containsIgnoreCase(line, "WARN")) {
            printColoredAtomic(line, ConsoleColor::Yellow);
            return;
        } else if(containsIgnoreCase(line, "DEBUG")) {
            printColoredAtomic(line, ConsoleColor::Cyan);
            return;
        }
        printColoredAtomic(line, ConsoleColor::Default);
    };

    // stderr 处理回调
    auto processStderr = [](const std::string& line) {
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
            std::string dotted = m[1].str();
            std::string slashed = dotted;
            std::replace(slashed.begin(), slashed.end(), '.', '/');
            slashed += ".py";

            out += "File \"" + slashed + "\", line " + m[2].str();

            lastPos = m.position() + m.length();
        }

        // 拼接剩余部分
        out.append(line, lastPos);

        printColoredAtomic(out, ConsoleColor::Red);
    };

    // ===================== 用户配置后置处理 =====================
    // 是否过滤非Python输出
    bool filterPython = userConfig.value("include_debug_mod", true);
    // 调试器端口（0为不启用）
    int debuggerPort = _GET_ENV_DEBUGGER_PORT();
    if(debuggerPort == 0) {
        // 解析用户配置覆盖
        auto debuggerConfig = userConfig.value("modpc_debugger", nlohmann::json::object());
        if(debuggerConfig.is_object()) {
            bool debuggerEnabled = debuggerConfig.value("enabled", false);
            if(debuggerEnabled) {
                debuggerPort = debuggerConfig.value("port", 5632);
            }
        }
    }

    // 启动两个线程并行读取（避免任何死锁）
    std::thread tOut(
        readPipeThread,
        outRead,
        filterPython,
        std::function<void(const std::string&)>(processStdout)
    );

    std::thread tErr(
        readPipeThread,
        errRead,
        filterPython,
        std::function<void(const std::string&)>(processStderr)
    );

    if(debuggerPort > 0) {
        // 尝试启动mcdbg调试器附加
        debuggerAttachToProcess(pid, debuggerPort);
    }

    if(autoHotReload && modDirList != nullptr) {
        // 启动热更新追踪任务
        reloadTask.setProcessId(pid);
        // 输出追踪目录列表
        std::cout << "[HotReload] 追踪目录列表：\n";
        for(const auto& modDirConfig : *modDirList) {
            if(modDirConfig.hotReload) {
                std::cout << "  -> " << modDirConfig.getAbsoluteU8String() << "\n";
            }
        }
        reloadTask.setModDirs(UserModDirConfig::toPathList(*modDirList));
        reloadTask.start();
    }
    styleProcessor.start();

    // 等待子进程退出（子进程退出后会关闭写端，使 ReadFile 返回 ERROR_BROKEN_PIPE）
    WaitForSingleObject(pi.hProcess, INFINITE);

    // 停止热更新任务
    reloadTask.safeExit();
    // 停止IPC服务器 如果已启用
    ipcServer->safeExit();
    // 停止样式处理器
    styleProcessor.safeExit();

    // 等待读线程退出并关闭读端句柄
    tOut.join();
    tErr.join();

    CloseHandle(outRead);
    CloseHandle(errRead);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

#endif

// 尝试更新游戏路径
static bool updateGamePath(std::filesystem::path& path) {
    auto autoExePath = MCDevTool::autoMatchLatestGameExePath();
    if(autoExePath.has_value()) {
        path = std::move(autoExePath.value());
        return true;
    }
    return false;
}

// 尝试更新用户游戏路径配置
static void tryUpdateUserGamePath(const std::filesystem::path& newPath) {
    auto configPath = std::filesystem::current_path() / ".mcdev.json";
    std::ifstream configFile(configPath, std::ios::binary);
    std::string content((std::istreambuf_iterator<char>(configFile)),
                        std::istreambuf_iterator<char>());
    nlohmann::json config = nlohmann::json::parse(content, nullptr, false, true);
    configFile.close();
    if(config.is_discarded()) {
        throw std::runtime_error("配置文件解析失败，JSON异常，请检查格式是否正确。");
    }
    auto u8Path = newPath.generic_u8string();
    if constexpr (sizeof(char8_t) == sizeof(char)) {
        config["game_executable_path"] = reinterpret_cast<const char*>(u8Path.c_str());
    } else {
        config["game_executable_path"] = std::string(u8Path.begin(), u8Path.end());
    }
    // 重新写入配置文件
    std::ofstream outConfigFile(configPath, std::ios::binary | std::ios::trunc);
    outConfigFile << config.dump(4);
    outConfigFile.close();
}

// 获取环境变量强制覆写的自动进入游戏存档状态 -1.未设置 0.关闭 1.开启
static int _GET_ENV_AUTO_JOIN_GAME_STATE() {
    auto* autoJoin = std::getenv("MCDEV_AUTO_JOIN_GAME");
    if(autoJoin == nullptr) {
        return -1;
    }
    std::string_view valStr(autoJoin);
    if(valStr == "0" || valStr == "false" || valStr == "False") {
        return 0;
    } else if(valStr == "1" || valStr == "true" || valStr == "True") {
        return 1;
    }
    return -1;
}

// 启动游戏
static void startGame(const nlohmann::json& config) {
    auto gameExePath = std::filesystem::u8path(config.value("game_executable_path", ""));
    if(!std::filesystem::is_regular_file(gameExePath)) {
        // 游戏exe路径无效 重新搜索新版
        if(updateGamePath(gameExePath)) {
            std::cout << "游戏路径无效，重新搜索：" << gameExePath.generic_string() << "\n";
            std::string u8input;
            std::cout << "是否更新配置文件中的游戏路径？(Y/N)：";
            std::getline(std::cin, u8input);
            if(u8input == "Y" || u8input == "y") {
                tryUpdateUserGamePath(gameExePath);
                std::cout << "已更新配置文件中的游戏路径。\n";
            } else {
                // std::cout << "未更新配置文件中的游戏路径。\n";
                throw std::runtime_error("未更新配置文件中的游戏路径，启动终止。");
            }
        } else {
            // std::cerr << "未能找到有效的游戏exe文件。\n";
            throw std::runtime_error("未能找到有效的游戏exe文件。");
        }
    }

    if(_GET_ENV_IS_SUBPROCESS_MODE()) {
        launchGameExe(gameExePath, "", config, nullptr);
        return;
    }

    MCDevTool::cleanRuntimePacks();
    std::vector<MCDevTool::Addon::PackInfo> linkedPacks;

    auto modDirConfigs = UserModDirConfig::parseListFromJson(
        config.value("included_mod_dirs", nlohmann::json::array({"./"})));

    if(config.value("include_debug_mod", true)) {
        auto debugMod = registerDebugMod(config, modDirConfigs);
        std::cout << "已注册调试MOD：" << debugMod.uuid << "\n";
        linkedPacks.push_back(std::move(debugMod));
        linkUserConfigModDirs(modDirConfigs, linkedPacks);
    } else {
        linkUserConfigModDirs(modDirConfigs, linkedPacks);
    }

    // 创建世界
    auto worldFolderName = config.value("world_folder_name", "MC_DEV_WORLD");
    auto resetWorld = config.value("reset_world", false);   // 若启用该参数 每次都会强制覆盖世界
    auto worldsPath = MCDevTool::getMinecraftWorldsPath() / std::filesystem::u8path(worldFolderName);
    if(!std::filesystem::is_directory(worldsPath) || resetWorld) {
        std::filesystem::remove_all(worldsPath);
        if(resetWorld) {
            std::cout << "已删除旧世界数据，正在创建新世界...\n";
        }
        std::filesystem::create_directories(worldsPath);
        std::ofstream levelFile(worldsPath / "level.dat", std::ios::binary);
        auto levelDat = createUserLevel(config);
        levelFile.write(reinterpret_cast<const char*>(levelDat.data()), levelDat.size());
        levelFile.close();
    } else {
        // 更新level.dat的配置数据
        MCDevTool::Level::updateLevelDatLastPlayedInFile(worldsPath / "level.dat");
        // auto levelOptions = parseLevelOptionsFromUserConfig(config);
        // MCDevTool::Level::updateLevelDatWorldDataInFile(worldsPath / "level.dat",
        //     std::nullopt, levelOptions
        // );
    }

    // 生成清单文件 netease_world_behavior_packs.json / netease_world_resource_packs.json
    auto behPacksManifest = nlohmann::json::array();
    auto resPacksManifest = nlohmann::json::array();
    for(const auto& pack : linkedPacks) {
        nlohmann::json packEntry {
            { "pack_id", pack.uuid },
            { "version", nlohmann::json::parse(pack.version) },
        };
        if(pack.type == MCDevTool::Addon::PackType::BEHAVIOR) {
            behPacksManifest.push_back(std::move(packEntry));
        } else if(pack.type == MCDevTool::Addon::PackType::RESOURCE) {
            resPacksManifest.push_back(std::move(packEntry));
        }
    }
    
    auto autoJoinGame = config.value("auto_join_game", true);
    auto envAutoJoin = _GET_ENV_AUTO_JOIN_GAME_STATE();
    if (envAutoJoin != -1) {
        // 环境变量覆写配置文件
        autoJoinGame = (envAutoJoin == 1);
    }
    std::string targetBehJson = "netease_world_behavior_packs.json";
    std::string targetResJson = "netease_world_resource_packs.json";
    if(autoJoinGame) {
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

// #ifdef MCDEV_EXPERIMENTAL_LAUNCH_WITH_CONFIG_PATH
    if(!autoJoinGame) {
        // 不自动进入游戏 直接启动游戏exe
        launchGameExe(gameExePath, "", config, &modDirConfigs);
        return;
    }
    
    auto configPath = worldsPath / "dev_config.cppconfig";
    // 创建dev_config
    nlohmann::json devConfig {
        {"world_info", {
            {"level_id", worldFolderName}
        }},
        {"room_info", nlohmann::json::object()},
        {"player_info", {
            {"urs", ""},
            {"user_id", 0},
            {"user_name", config.value("user_name", "developer") },
        }},
    };
    if(config.contains("skin_info") && config["skin_info"].is_object()) {
        // 用户自定义skin_info
        devConfig["skin_info"] = config["skin_info"];
    } else {
        // 自动生成skin_info
        devConfig["skin_info"] = {
            {"slim", false},
            {"skin", (gameExePath.parent_path() / "data/skin_packs/vanilla/steve.png").generic_string() }
        };
    }
    std::ofstream configFile(configPath);
    configFile << devConfig.dump(4);
    configFile.close();
    launchGameExe(gameExePath, configPath.generic_string(), config, &modDirConfigs);
// #else
//     launchGameExe(gameExePath);
// #endif
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
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#else
int main(int argc, char* argv[]) {
#endif

    if (_GET_ENV_MCDEV_OUTPUT_MODE() == 1) {
        setvbuf(stdout, nullptr, _IONBF, 0);
    }

    #ifdef NDEBUG
    try {
    #endif
        #ifdef MCDK_ENABLE_CLI
        if(argc > 1) {
            return MCDK_CLI_PARSE(argc, argv);
        }
        #endif
        auto config = userParseConfig();
        startGame(config);
    #ifdef NDEBUG
    } catch(const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n";
        return 1;
    }
    #endif
    return 0;
}