#include <mcdk/application.hpp>
#include <mcdk/config.hpp>
#include <mcdk/game_process.hpp>

#include <mcdk/env.hpp>
#include <mcdk/level.hpp>
#include <mcdk/mod_register.hpp>
#include <mcdk/world_project.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <mcdevtool/env.h>
#include <mcdevtool/level.h>
#include <mcdevtool/utils.h>
#include <nlohmann/json.hpp>

using mcdk::UserModDirConfig;

void mcdk::startGame(const UserConfig& config) {
    auto gameExePath = config.gameExecutablePath;
    if (!std::filesystem::is_regular_file(gameExePath)) {
        // 游戏 exe 路径无效，重新发现并选择版本
        if (mcdk::updateGamePath(gameExePath)) {
            mcdk::tryUpdateUserGamePath(gameExePath);
            std::cout << "已更新配置文件中的游戏路径。\n";
        } else {
            throw std::runtime_error("未能找到有效的游戏exe文件。");
        }
    }

    auto _isSubprocessMode = mcdk::getEnvIsSubprocessMode();

    if (!_isSubprocessMode) {
        MCDevTool::cleanRuntimePacks();
    }

    auto                                    modDirConfigs          = config.modDirectories;
    const auto                              worldSourcePath        = mcdk::resolveWorldSourcePath(config.world.source);
    auto                                    hotReloadModDirConfigs = modDirConfigs;
    std::vector<MCDevTool::Addon::PackInfo> worldPacks;
    if (worldSourcePath) {
        // 地图源目录只加入热更新列表，不参与原有 Addon 扫描和全局链接。
        worldPacks = mcdk::collectWorldPacks(*worldSourcePath);
        mcdk::appendWorldHotReloadModDir(hotReloadModDirConfigs, *worldSourcePath);
        std::cout << "[MCDK] World Source " << MCDevTool::Utils::pathToGenericUtf8(*worldSourcePath)
                  << "  Packs=" << worldPacks.size() << "\n";
    }

    if (_isSubprocessMode) {
        // 子进程模式 直接启动游戏exe（通常由vsc插件多开使用）
        launchGameExe(gameExePath, "", config, &hotReloadModDirConfigs, &worldPacks);
        return;
    }
    std::vector<MCDevTool::Addon::PackInfo> linkedPacks;
    if (config.includeDebugMod) {
        auto debugMod = mcdk::registerDebugMod();
        std::cout << "[MCDK] Addons\n";
        std::cout << "  Debug    UUID=" << debugMod.uuid << "\n";
        linkedPacks.push_back(std::move(debugMod));
        mcdk::linkUserConfigModDirs(modDirConfigs, linkedPacks);
    } else {
        std::cout << "[MCDK] Addons\n";
        mcdk::linkUserConfigModDirs(modDirConfigs, linkedPacks);
    }
    // 地图包只补充热更新所需的源码路径，不参与全局链接或世界包清单合并。
    auto hotReloadPacks = mcdk::makeHotReloadPacks(linkedPacks, worldPacks);
    // 创建世界
    const auto& worldFolderName = config.world.folderName;
    const bool  resetWorld      = config.world.reset; // 若启用该参数 每次都会强制覆盖世界
    auto        worldsPath      = MCDevTool::getMinecraftWorldsPath() / std::filesystem::u8path(worldFolderName);
    if (worldSourcePath) {
        // 玩法地图使用源码世界作为初始数据，并在后续启动中保留运行时进度。
        const bool deployed = mcdk::deployWorldSource(*worldSourcePath, worldsPath, resetWorld);
        if (deployed) {
            std::cout << "已从玩法地图源码部署运行时世界数据。\n";
        }
    } else if (!std::filesystem::is_directory(worldsPath) || resetWorld) {
        std::filesystem::remove_all(worldsPath);
        if (resetWorld) {
            std::cout << "已删除旧世界数据，正在创建新世界...\n";
        }
        std::filesystem::create_directories(worldsPath);
        std::ofstream levelFile(worldsPath / "level.dat", std::ios::binary);
        auto          levelDat = mcdk::createUserLevel(config.world);
        levelFile.write(reinterpret_cast<const char*>(levelDat.data()), levelDat.size());
        levelFile.close();
    } else {
        // 更新level.dat的配置数据
        if (mcdk::getEnvIsPluginEnv()) {
            // 插件环境每次启动都要覆盖配置
            MCDevTool::Level::updateLevelDatWorldDataInFile(worldsPath / "level.dat", std::nullopt, config.world.level);
        } else {
            // 非插件环境只更新时间戳
            MCDevTool::Level::updateLevelDatLastPlayedInFile(worldsPath / "level.dat");
        }
    }

    // netease_world_behavior_packs.json / netease_world_resource_packs.json
    auto autoJoinGame = config.world.autoJoin;
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
    auto behPacksManifest =
        worldSourcePath
            ? mcdk::loadWorldPackManifest(*worldSourcePath, MCDevTool::Addon::PackType::BEHAVIOR, targetBehJson)
            : mcdk::WorldPackManifest{};
    auto resPacksManifest =
        worldSourcePath
            ? mcdk::loadWorldPackManifest(*worldSourcePath, MCDevTool::Addon::PackType::RESOURCE, targetResJson)
            : mcdk::WorldPackManifest{};
    mcdk::mergeLinkedPacksIntoManifest(behPacksManifest, resPacksManifest, linkedPacks);
    mcdk::writeWorldPackManifest(worldsPath / targetBehJson, behPacksManifest);
    mcdk::writeWorldPackManifest(worldsPath / targetResJson, resPacksManifest);

    if (!autoJoinGame) {
        // 不自动进入游戏 直接启动游戏exe
        launchGameExe(gameExePath, "", config, &hotReloadModDirConfigs, &hotReloadPacks);
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
             {"user_name", config.player.name},
         }},
    };

    auto defaultSkinPath =
        MCDevTool::Utils::pathToGenericUtf8(gameExePath.parent_path() / "data/skin_packs/vanilla/steve.png");

    if (config.player.skin) {
        auto skinPath = config.player.skin->path;
        if (skinPath.empty()) {
            skinPath = std::filesystem::u8path(defaultSkinPath);
        }
        if (!std::filesystem::is_regular_file(skinPath)) {
            throw std::runtime_error("自定义皮肤文件不存在：" + MCDevTool::Utils::pathToGenericUtf8(skinPath));
        }
        devConfig["skin_info"] = {
            {"slim", config.player.skin->slim},
            {"skin", MCDevTool::Utils::pathToGenericUtf8(skinPath)},
        };
    } else {
        // 自动生成skin_info
        devConfig["skin_info"] = {{"slim", false}, {"skin", std::move(defaultSkinPath)}};
    }
    std::ofstream configFile(configPath);
    configFile << devConfig.dump(4);
    configFile.close();
    launchGameExe(
        gameExePath,
        MCDevTool::Utils::pathToGenericUtf8(configPath),
        config,
        &hotReloadModDirConfigs,
        &hotReloadPacks
    );
}
