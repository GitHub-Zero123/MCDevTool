#include <iostream>
#include <string>
#include <fstream>
#include <iterator>
#include <filesystem>
// std::move
#include <utility>
#include <vector>
#include <optional>
#include <stdexcept>
#include <cstdint>
#include <INCLUDE_MOD.h>
#include <mcdevtool/env.h>
#include <mcdevtool/addon.h>
#include <mcdevtool/level.h>
#include <nlohmann/json.hpp>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static nlohmann::json createDefaultConfig() {
    std::string u8input;
    std::cout << "请输入游戏可执行文件路径：";
    std::getline(std::cin, u8input);
    if(u8input.size() > 2 && u8input[0] == '"' && u8input[u8input.size() - 1] == '"') {
        // 字符串形式的路径，去掉头尾部引号
        u8input = u8input.substr(1, u8input.size() - 2);
    }
    std::filesystem::path exePath = std::filesystem::u8path(u8input);
    if(!std::filesystem::is_regular_file(exePath)) {
        std::cerr << "路径无效，文件不存在。\n";
        exit(1);
    }
    nlohmann::json config {
        // 包含的mod目录，支持多个
        {"included_mod_dirs", nlohmann::json::array({"./"})},
        // 世界随机种子 留空则随机生成
        {"world_seed", nullptr},
        // 是否重置世界
        {"reset_world", false},
        // 世界名称
        {"world_name", "MC_DEV_WORLD"},
        // 世界文件夹名称
        {"world_folder_name", "MC_DEV_WORLD"},
        // 是否自动进入游戏存档
        {"auto_join_game", true},
        // 包含调试模组(提供R键热更新以及py输出流标记)
        {"include_debug_mod", true},
        // 世界类型 0-旧版 1-无限 2-平坦
        {"world_type", 1},
        // 游戏模式 0-生存 1-创造 2-冒险
        {"game_mode", 1},
        // 是否启用作弊
        {"enable_cheats", true},
        // 保留物品栏
        {"keep_inventory", true}
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
MCDevTool::Addon::PackInfo registerDebugMod() {
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
    std::string uuidNoDash;
    for(char c : info.uuid) {
        if(c != '-') {
            uuidNoDash.push_back(c);
        }
    }
    auto target = outDir / uuidNoDash;
    // 写入ADDON数据到目标目录
    if(std::filesystem::exists(target)) {
        std::filesystem::remove_all(target);
    }
    for(const auto& [resName, resData] : INCLUDE_MOD_RES::resourceMap) {
        auto resPath = target / std::filesystem::u8path(resName);
        std::filesystem::create_directories(resPath.parent_path());
        std::ofstream resFile(resPath, std::ios::binary);
        resFile.write(reinterpret_cast<const char*>(resData.first), resData.second);
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
                std::cout << "LINK行为包: '" << info.name << "', UUID: " << info.uuid << "\n";
            } else if(info.type == MCDevTool::Addon::PackType::RESOURCE) {
                std::cout << "LINK资源包: '" << info.name << "', UUID: " << info.uuid << "\n";
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

// 启动游戏exe并附着终端
// static void launchGameExe(const std::filesystem::path& exePath, std::string_view config="") {
//     STARTUPINFOA si;
//     PROCESS_INFORMATION pi;
//     ZeroMemory(&si, sizeof(si));
//     si.cb = sizeof(si);
//     ZeroMemory(&pi, sizeof(pi));

//     char* cmdLine = nullptr;
//     if(!config.empty()) {
//         std::string configArg = " config=" + std::string(config);
//         cmdLine = configArg.data();
//     }

//     // 创建进程
//     if(!CreateProcessA(
//         exePath.string().c_str(),   // 可执行文件路径
//         cmdLine,  // 命令行参数
//         nullptr,                    // 进程属性
//         nullptr,                    // 线程属性
//         FALSE,                      // 是否继承句柄
//         0,                        // 创建标志
//         nullptr,                    // 使用父进程的环境变量
//         nullptr,               // 使用父进程的工作目录
//         &si,                        // 指向 STARTUPINFO 结构体的指针
//         &pi                  // 指向 PROCESS_INFORMATION 结构体的指针
//     )) {
//         throw std::runtime_error("无法启动游戏可执行文件，CreateProcess失败。");
//     }

//     // 等待进程结束
//     WaitForSingleObject(pi.hProcess, INFINITE);

//     // 关闭进程和线程句柄
//     CloseHandle(pi.hProcess);
//     CloseHandle(pi.hThread);
// }

static void launchGameExe(const std::filesystem::path& exePath, std::string_view config="") {
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};

    std::string cmd = "\"" + exePath.string() + "\"";
    if (!config.empty()) {
        cmd += " config=\"" + std::string(config) + "\"";
    }

    std::vector<char> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back('\0');

    if (!CreateProcessA(
        nullptr,
        cmdBuf.data(),
        nullptr, nullptr,
        FALSE,
        0,
        nullptr,
        exePath.parent_path().string().c_str(),   // ✅ 设置工作目录为游戏目录
        &si,
        &pi
    )) {
        throw std::runtime_error("无法启动游戏可执行文件，CreateProcess失败。");
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
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

// 启动游戏
static void startGame(const nlohmann::json& config) {
    auto gameExePath = std::filesystem::u8path(config.value("game_executable_path", ""));
    if(!std::filesystem::is_regular_file(gameExePath)) {
        throw std::runtime_error("游戏可执行文件路径无效，文件不存在。");
    }
    MCDevTool::cleanRuntimePacks();
    std::vector<MCDevTool::Addon::PackInfo> linkedPacks;
    // link debug MOD
    if(config.value("include_debug_mod", true)) {
        auto debugMod = registerDebugMod();
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

    std::ofstream behManifestFile(worldsPath / "netease_world_behavior_packs.json");;
    behManifestFile << behPacksManifest.dump(4);
    behManifestFile.close();

    std::ofstream resManifestFile(worldsPath / "netease_world_resource_packs.json");
    resManifestFile << resPacksManifest.dump(4);
    resManifestFile.close();

#ifdef MCDEV_EXPERIMENTAL_LAUNCH_WITH_CONFIG_PATH
    auto autoJoinGame = config.value("auto_join_game", true);;
    if(!autoJoinGame) {
        launchGameExe(gameExePath);
        return;
    }
    auto configPath = worldsPath / "dev_config.cppconfig";
    // 创建dev_config
    nlohmann::json devConfig {
        // {"version", "3.6.0.129998"},
        {"world_info", {
            {"level_id", worldFolderName}
        }},
        // 生成自己的path
        {"path", configPath.generic_string()}
    };
    std::ofstream configFile(configPath);
    configFile << devConfig.dump(4);
    configFile.close();
    launchGameExe(gameExePath, configPath.generic_string());
#else
    launchGameExe(gameExePath);
#endif
}

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    #ifdef NDEBUG
    try {
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
