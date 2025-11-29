#include <iostream>
#include <string>
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
#include <optional>
#include <stdexcept>
#include <cstdint>
#include <algorithm>
#include <INCLUDE_MOD.h>
#include <mcdevtool/env.h>
#include <mcdevtool/addon.h>
#include <mcdevtool/level.h>
#include <nlohmann/json.hpp>

#define MCDEV_EXPERIMENTAL_LAUNCH_WITH_CONFIG_PATH

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// 字符串关键字替换
static void stringReplace(std::string& str, const std::string& from, const std::string& to) {
    size_t start_pos = 0;
    while((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
}

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
        // 世界类型 0-旧版 1-无限 2-平坦
        { "world_type", 1 },
        // 游戏模式 0-生存 1-创造 2-冒险
        { "game_mode", 1 },
        // 是否启用作弊
        { "enable_cheats", true },
        // 保留物品栏
        { "keep_inventory", true },
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
MCDevTool::Addon::PackInfo registerDebugMod(const nlohmann::json& config) {
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
    // 生成格式化的字面量json
    auto DEBUG_OPTIONS = config.value("debug_options", nlohmann::json::object()).dump();
    // 替换为python boolean格式
    stringReplace(DEBUG_OPTIONS, "true", "True");
    stringReplace(DEBUG_OPTIONS, "false", "False");
    stringReplace(DEBUG_OPTIONS, "null", "None");
    for(const auto& [resName, resData] : INCLUDE_MOD_RES::resourceMap) {
        auto resPath = target / std::filesystem::u8path(resName);
        std::filesystem::create_directories(resPath.parent_path());
        std::ofstream resFile(resPath, std::ios::binary);
        if(resName.ends_with("modMain.py")) {
            // 替换关键字实现传参
            std::string modMainContent(reinterpret_cast<const char*>(resData.first), resData.second);
            stringReplace(modMainContent, "\"{#debug_options}\"", DEBUG_OPTIONS);
            resFile.write(modMainContent.data(), modMainContent.size());
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

// 根据用户config创建level.dat
static std::vector<uint8_t> createUserLevel(const nlohmann::json& config) {
    MCDevTool::Level::LevelOptions options;
    options.worldType = static_cast<uint32_t>(config.value("world_type", 1));
    options.gameMode = static_cast<uint32_t>(config.value("game_mode", 1));
    if(config.contains("world_seed") && !config["world_seed"].is_null()) {
        options.seed = config["world_seed"].get<uint64_t>();
    }
    options.enableCheats = config.value("enable_cheats", true);
    options.keepInventory = config.value("keep_inventory", true);
    std::string worldName = config.value("world_name", "MC_DEV_WORLD");
    // std::string folderName = config.value("world_folder_name", "MC_DEV_WORLD");
    auto levelDat = MCDevTool::Level::createDefaultLevelDat(worldName, options);
    return levelDat;
}

#ifdef MCDEV_EXPERIMENTAL_LAUNCH_WITH_CONFIG_PATH

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
        std::cout << msg << "\n";
        return;
    }

    CONSOLE_SCREEN_BUFFER_INFO info;
    if (!GetConsoleScreenBufferInfo(hConsole, &info)) {
        std::cout << msg << "\n";
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

    std::cout << msg << "\n";

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

// 读 pipe 的线程函数
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
                // 其它错误可以选择退出或重试，这里退出
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

static void launchGameExe(
    const std::filesystem::path& exePath,
    std::string_view config = "",
    bool filterPython = false
) {
    STARTUPINFOA si = { sizeof(si) };
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
    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back('\0');

    if (!CreateProcessA(
        nullptr,
        cmdBuf.data(),
        nullptr, nullptr,
        TRUE,   // 继承句柄
        0,
        nullptr,
        nullptr,
        &si,
        &pi
    )) {
        CloseHandle(outRead); CloseHandle(outWrite);
        CloseHandle(errRead); CloseHandle(errWrite);
        throw std::runtime_error("CreateProcessA failed");
    }

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
        if(line.find("SUC") != std::string::npos) {
            printColoredAtomic(line, ConsoleColor::Green);
            return;
        } else if(line.find("[INFO][Developer]") != std::string::npos) {
            printColoredAtomic(line, ConsoleColor::DarkGray);
            return;
        } else if(line.find("ERROR") != std::string::npos) {
            printColoredAtomic(line, ConsoleColor::Red);
            return;
        } else if(line.find("WARN") != std::string::npos) {
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

            // 动态构造替换内容（你的 lambda 逻辑）
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
    // auto processStderr = [](const std::string& line) {
    //     printColoredAtomic(line, ConsoleColor::Red);
    // };

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

    // 等待子进程退出（子进程退出后会关闭写端，使 ReadFile 返回 ERROR_BROKEN_PIPE）
    WaitForSingleObject(pi.hProcess, INFINITE);

    // 等待读线程退出并关闭读端句柄
    tOut.join();
    tErr.join();

    CloseHandle(outRead);
    CloseHandle(errRead);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
}

#else

// 启动游戏exe并附着终端
static void launchGameExe(const std::filesystem::path& exePath) {
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    // 创建进程
    if(!CreateProcessA(
        exePath.string().c_str(),   // 可执行文件路径
        nullptr,                    // 命令行参数
        nullptr,                    // 进程属性
        nullptr,                    // 线程属性
        FALSE,                      // 是否继承句柄
        0,                          // 创建标志
        nullptr,                    // 使用父进程的环境变量
        nullptr,                    // 使用父进程的工作目录
        &si,                        // 指向 STARTUPINFO 结构体的指针
        &pi                         // 指向 PROCESS_INFORMATION 结构体的指针
    )) {
        throw std::runtime_error("无法启动游戏可执行文件，CreateProcess失败。");
    }

    // 等待进程结束
    WaitForSingleObject(pi.hProcess, INFINITE);

    // 关闭进程和线程句柄
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
                std::cout << "未更新配置文件中的游戏路径。\n";
            }
        } else {
            // std::cerr << "未能找到有效的游戏exe文件。\n";
            throw std::runtime_error("未能找到有效的游戏exe文件。");
        }
    }
    MCDevTool::cleanRuntimePacks();
    std::vector<MCDevTool::Addon::PackInfo> linkedPacks;
    // link debug MOD
    if(config.value("include_debug_mod", true)) {
        auto debugMod = registerDebugMod(config);
        std::cout << "已注册调试MOD：" << debugMod.uuid << "\n";
        linkedPacks.push_back(std::move(debugMod));
    }

    // link 用户MOD目录
    auto modDirs = config.value("included_mod_dirs", std::vector<std::string>{});
    linkUserMods(modDirs, linkedPacks);

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
        // 更新level.dat的最后游玩时间 确保在游戏内显示为最新游玩
        MCDevTool::Level::updateLevelDatLastPlayedInFile(worldsPath / "level.dat");
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
    // 检查是否附加debugmod
    bool useDebugMode = config.value("include_debug_mod", true);

#ifdef MCDEV_EXPERIMENTAL_LAUNCH_WITH_CONFIG_PATH
    if(!autoJoinGame) {
        launchGameExe(gameExePath, "", useDebugMode);
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
    launchGameExe(gameExePath, configPath.generic_string(), useDebugMode);
#else
    launchGameExe(gameExePath);
#endif
}

#ifdef MCDK_ENABLE_CLI
int MCDK_CLI_PARSE(int argc, wchar_t* argv[]);
#else
int MCDK_CLI_PARSE(int argc, char* argv[]);
#endif

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#else
int main(int argc, char* argv[]) {
#endif
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