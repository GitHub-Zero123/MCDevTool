#include <mcdk/application.hpp>
#include <mcdk/config.hpp>
#include <mcdk/console_output.hpp>
#include <mcdk/game_process.hpp>

#include <mcdk/env.hpp>
#include <mcdk/level.hpp>
#include <mcdk/mod_register.hpp>
#include <mcdk/plugin_declarations.hpp>
#include <mcdk/plugin_host/host.hpp>
#include <mcdk/plugin_host/world_request.hpp>
#include <mcdk/world_project.hpp>

#include <algorithm>
#include <exception>
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

namespace {
    // 插件终结的作用域守卫。
    // 析构里绝不能让异常逃出去：它多半是在栈展开途中运行的，再抛一次就是
    struct PluginScope {
        ~PluginScope() {
            try {
                mcdk::plugin_host::instance().shutdown();
            } catch (const std::exception& exception) {
                std::cerr << "[ERROR] 插件终结失败: " << exception.what() << '\n';
            } catch (...) {
                std::cerr << "[ERROR] 插件终结失败\n";
            }
        }
    };

    // 游戏一退出就摘掉存档里的链接。
    struct WorldPackLinkScope {
        std::filesystem::path world;

        ~WorldPackLinkScope() {
            try {
                mcdk::removeWorldPackLinks(world);
            } catch (const std::exception& exception) {
                std::cerr << "[ERROR] 存档链接清理失败，请勿在游戏内删除该存档: " << exception.what() << '\n';
            }
        }
    };
} // namespace

mcdk::GameRunResult mcdk::runGame(const UserConfig& config) {
    // 插件只从 .mcdev.json 的 plugins 声明加载，宿主不扫描任何目录。
    // 相对路径以 .mcdev.json 所在目录（即当前工作目录）为基准。
    auto& pluginHost   = mcdk::plugin_host::instance();
    auto  declarations = config.plugins;
    // 定制版编辑器经 MCDEV_PLUGINS 注入的插件，追加在项目声明之后，同样按 priority 排。
    if (auto external = mcdk::plugin_host::detail::parseEnvPluginDeclarations(); !external.empty()) {
        std::string paths;
        for (const auto& declaration : external) {
            paths += (paths.empty() ? "" : ", ") + declaration.path;
        }
        mcdk::printColoredAtomic("[MCDK] 外部插件（MCDEV_PLUGINS）：" + paths, mcdk::ConsoleColor::Cyan);
        declarations.insert(declarations.end(), external.begin(), external.end());
        std::stable_sort(declarations.begin(), declarations.end(), [](const auto& left, const auto& right) {
            return left.priority < right.priority;
        });
    }
    pluginHost.loadDeclared(declarations, std::filesystem::current_path());
    // 无论 startGame 怎么退出——正常结束、被插件否决、或中途抛异常——插件都必须
    // 走完终结流程。正常路径上 launchGameExe 已经做过，Host::shutdown 幂等，
    const PluginScope pluginScope;

    pluginHost.advance(MCDK_STAGE_REGISTER);
    return mcdk::startGame(config);
}

mcdk::GameRunResult mcdk::startGame(const UserConfig& userConfig) {
    auto gameExePath = userConfig.gameExecutablePath;
    if (!std::filesystem::is_regular_file(gameExePath)) {
        // 游戏 exe 路径无效，重新发现并选择版本
        if (mcdk::updateGamePath(gameExePath)) {
            mcdk::tryUpdateUserGamePath(gameExePath);
            std::cout << "已更新配置文件中的游戏路径。\n";
        } else {
            throw std::runtime_error("未能找到有效的游戏exe文件。");
        }
    }

    // 配置已完成解析与校验，插件可在此阶段读取它。
    mcdk::plugin_host::instance().advance(MCDK_STAGE_CONFIG);

    auto _isSubprocessMode = mcdk::getEnvIsSubprocessMode();

    // 插件可以改写存档设置（04-events.md §4.5）。子进程模式不处理存档，也就不问。
    // 没人改写时不拷贝配置。
    std::optional<UserConfig> pluginWorldConfig;
    if (!_isSubprocessMode) {
        if (auto overridden = mcdk::plugin_host::resolveWorld(userConfig.world)) {
            pluginWorldConfig        = userConfig;
            pluginWorldConfig->world = std::move(overridden->world);
            const auto& world        = pluginWorldConfig->world;
            std::cout << "[MCDK] 插件 " << overridden->ownerId << " 改写了存档设置（"
                      << (overridden->replaced ? "完全覆盖" : "按键覆盖") << "）：" << world.folderName
                      << (world.reset ? "，每次重置" : "") << "\n";
        }
    }
    const UserConfig& config = pluginWorldConfig ? *pluginWorldConfig : userConfig;

    // 存档级链接不碰全局目录，别的实例才不受影响。
    const bool linkToWorld = config.packLinkMode == PackLinkMode::World;
    if (!_isSubprocessMode && !linkToWorld) {
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
        return launchGameExe(gameExePath, "", config, &hotReloadModDirConfigs, &worldPacks);
    }
    std::vector<MCDevTool::Addon::PackInfo> linkedPacks;
    const auto linkPacks = [&](const MCDevTool::RuntimePackRoots& roots) {
        std::cout << "[MCDK] Addons\n";
        if (config.includeDebugMod) {
            auto debugMod = mcdk::registerDebugMod(roots);
            std::cout << "  Debug    UUID=" << debugMod.uuid << "\n";
            linkedPacks.push_back(std::move(debugMod));
        }
        mcdk::linkUserConfigModDirs(modDirConfigs, linkedPacks, roots);
    };
    if (!linkToWorld) {
        linkPacks(MCDevTool::getGlobalPackRoots());
    }
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

    // 存档可能刚被重置，存档级链接要等它就绪后再建。
    std::optional<WorldPackLinkScope> worldPackLinks;
    if (linkToWorld) {
        const auto roots = mcdk::prepareWorldPackRoots(worldsPath);
        worldPackLinks.emplace(worldsPath);
        linkPacks(roots);
        mcdk::recordWorldPackLinks(worldsPath, linkedPacks);
    }
    // 地图包只补充热更新所需的源码路径，不参与全局链接或世界包清单合并。
    auto hotReloadPacks = mcdk::makeHotReloadPacks(linkedPacks, worldPacks);

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
    // 世界目录与 pack 清单已就绪，落盘前的最后一个干预点。
    mcdk::plugin_host::instance().advance(MCDK_STAGE_WORLD);

    mcdk::mergeLinkedPacksIntoManifest(behPacksManifest, resPacksManifest, linkedPacks);
    mcdk::writeWorldPackManifest(worldsPath / targetBehJson, behPacksManifest);
    mcdk::writeWorldPackManifest(worldsPath / targetResJson, resPacksManifest);

    if (!autoJoinGame) {
        // 不自动进入游戏 直接启动游戏exe
        return launchGameExe(gameExePath, "", config, &hotReloadModDirConfigs, &hotReloadPacks);
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
    return launchGameExe(
        gameExePath,
        MCDevTool::Utils::pathToGenericUtf8(configPath),
        config,
        &hotReloadModDirConfigs,
        &hotReloadPacks
    );
}
