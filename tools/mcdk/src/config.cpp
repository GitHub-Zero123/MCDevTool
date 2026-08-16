#include <mcdk/config.hpp>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>

#include <mcdevtool/env.h>
#include <mcdevtool/utils.h>
#include <nlohmann/json.hpp>

#include <mcdk/env.hpp>

namespace mcdk {
    namespace {
        using Json = nlohmann::json;

        std::vector<UserModDirConfig> parseModDirectories(const Json& value) {
            if (!value.is_array()) {
                throw std::runtime_error("Mod directories configuration should be an array.");
            }

            std::vector<UserModDirConfig> directories;
            directories.reserve(value.size());
            for (const auto& item : value) {
                UserModDirConfig directory;
                if (item.is_string()) {
                    directory.path      = std::filesystem::u8path(item.get<std::string>());
                    directory.hotReload = true;
                } else if (item.is_object()) {
                    directory.path      = std::filesystem::u8path(item.value("path", "./"));
                    directory.hotReload = item.value("hot_reload", true);
                    directory.enabled   = item.value("enabled", true);
                } else {
                    throw std::runtime_error("Invalid mod directory configuration format.");
                }

                if (directory.enabled) {
                    directories.push_back(std::move(directory));
                }
            }
            return directories;
        }

        WorldSourceConfig parseWorldSource(const Json& root) {
            const auto setting = root.find("world_source_path");
            if (setting == root.end() || (setting->is_string() && setting->get<std::string>() == "auto")) {
                return {};
            }
            if (setting->is_null() || (setting->is_string() && setting->get<std::string>().empty())) {
                return {.mode = WorldSourceConfig::Mode::Disabled};
            }
            if (!setting->is_string()) {
                throw std::runtime_error("world_source_path 必须为路径字符串、auto 或 null。");
            }
            return {
                .mode = WorldSourceConfig::Mode::Path,
                .path = std::filesystem::u8path(setting->get<std::string>()),
            };
        }

        void parseLevelConfig(const Json& root, WorldProjectConfig& world) {
            auto& level           = world.level;
            level.worldType       = static_cast<uint32_t>(root.value("world_type", 1));
            level.gameMode        = static_cast<uint32_t>(root.value("game_mode", 1));
            level.enableCheats    = root.value("enable_cheats", true);
            level.keepInventory   = root.value("keep_inventory", true);
            level.doWeatherCycle  = root.value("do_weather_cycle", true);
            level.doDaylightCycle = root.value("do_daylight_cycle", true);

            if (const auto seed = root.find("world_seed"); seed != root.end() && !seed->is_null()) {
                level.seed = seed->get<uint64_t>();
            }

            const auto experiments = root.find("experiment_options");
            if (experiments == root.end() || !experiments->is_object()) {
                return;
            }
            auto& options                      = level.experimentsOptions;
            options.enable                     = true;
            options.dataDrivenBiomes           = experiments->value("data_driven_biomes", false);
            options.upcomingCreatorFeatures    = experiments->value("upcoming_creator_features", false);
            options.experimentalCreatorCameras = experiments->value("experimental_creator_cameras", false);
            options.gametest                   = experiments->value("gametest", false);
            options.deferredTechnicalPreview   = experiments->value("deferred_technical_preview", false);
        }

        void parseDebugOptions(const Json& root, DebugModOptions& options) {
            const auto value = root.find("debug_options");
            if (value == root.end() || !value->is_object()) {
                return;
            }
            // Serialize the README-defined object once; the embedded Python module is the only consumer of this data.
            options.serializedJson = value->dump();
        }

        GameLogProtocol parseLogProtocol(const Json& root) {
            const auto value = root.find("log_protocol");
            if (value == root.end()) {
                return GameLogProtocol::Stdio;
            }
            if (!value->is_number_integer()) {
                throw std::runtime_error("log_protocol must be an integer: 0 (stdio) or 1 (Safaia).");
            }

            switch (value->get<int>()) {
                case 0:
                    return GameLogProtocol::Stdio;
                case 1:
                    return GameLogProtocol::Safaia;
                default:
                    throw std::runtime_error("log_protocol must be 0 (stdio) or 1 (Safaia).");
            }
        }

        void parseWindowStyle(const Json& root, MCDevTool::Style::StyleConfig& style) {
            const auto value = root.find("window_style");
            if (value == root.end()) {
                return;
            }

            style.alwaysOnTop     = value->value("always_on_top", false);
            style.hideTitleBar    = value->value("hide_title_bar", false);
            style.hideTaskbarIcon = value->value("hide_taskbar_icon", false);

            if (const auto color = value->find("title_bar_color");
                color != value->end() && color->is_array() && color->size() >= 3) {
                style.titleBarColor = MCDevTool::Style::RgbColor{
                    .red   = (*color)[0].get<uint8_t>(),
                    .green = (*color)[1].get<uint8_t>(),
                    .blue  = (*color)[2].get<uint8_t>(),
                };
            }
            if (const auto opacity = value->find("opacity"); opacity != value->end() && opacity->is_number_integer()) {
                const int amount = opacity->get<int>();
                if (amount >= 0 && amount <= 255) {
                    style.windowOpacity = static_cast<uint8_t>(amount);
                }
            }
            if (const auto size = value->find("fixed_size");
                size != value->end() && size->is_array() && size->size() >= 2) {
                style.fixedSize = MCDevTool::Style::WindowSize{
                    .width  = (*size)[0].get<int>(),
                    .height = (*size)[1].get<int>(),
                };
            }
            if (const auto position = value->find("fixed_position");
                position != value->end() && position->is_array() && position->size() >= 2) {
                style.fixedPosition = MCDevTool::Style::WindowPosition{
                    .x = (*position)[0].get<int>(),
                    .y = (*position)[1].get<int>(),
                };
            }
            if (const auto corner = value->find("lock_corner"); corner != value->end() && corner->is_number_integer()) {
                style.lockCorner = static_cast<MCDevTool::Style::WindowCorner>(corner->get<int>());
            }
        }

        UserConfig parseUserConfigJson(const Json& root) {
            if (!root.is_object()) {
                throw std::runtime_error("配置文件根节点必须是 JSON 对象。");
            }

            UserConfig config;
            config.gameExecutablePath = std::filesystem::u8path(root.value("game_executable_path", ""));
            config.modDirectories     = parseModDirectories(root.value("included_mod_dirs", Json::array({"./"})));

            config.world.name       = root.value("world_name", "MC_DEV_WORLD");
            config.world.folderName = root.value("world_folder_name", "MC_DEV_WORLD");
            config.world.source     = parseWorldSource(root);
            config.world.reset      = root.value("reset_world", false);
            config.world.autoJoin   = root.value("auto_join_game", true);
            parseLevelConfig(root, config.world);

            config.player.name = root.value("user_name", "developer");
            if (const auto skin = root.find("skin_info"); skin != root.end() && skin->is_object()) {
                config.player.skin = SkinConfig{
                    .slim = skin->value("slim", false),
                    .path = std::filesystem::u8path(skin->value("skin", "")),
                };
            }

            config.includeDebugMod     = root.value("include_debug_mod", true);
            config.hotReload.mods      = root.value("auto_hot_reload_mods", true);
            config.hotReload.ui        = root.value("auto_hot_reload_ui", false);
            config.hotReload.shaders   = root.value("auto_hot_reload_shaders", false);
            config.hotReload.materials = root.value("auto_hot_reload_materials", false);
            config.hotReload.particles = root.value("auto_hot_reload_particles", false);
            parseDebugOptions(root, config.debugOptions);
            config.logProtocol = parseLogProtocol(root);

            if (const auto debugger = root.find("modpc_debugger"); debugger != root.end() && debugger->is_object()) {
                config.modPcDebugger.enabled = debugger->value("enabled", false);
                config.modPcDebugger.port    = debugger->value("port", 5632);
            }
            if (const auto debugger = root.find("ptvsd_debugger"); debugger != root.end() && debugger->is_object()) {
                config.ptvsdDebugger.enabled = debugger->value("enabled", false);
                config.ptvsdDebugger.ip      = debugger->value("ip", "localhost");
                config.ptvsdDebugger.port    = debugger->value("port", 56788);
            }

            parseWindowStyle(root, config.windowStyle);

            if (const auto netease = root.find("netease_config"); netease != root.end() && netease->is_object()) {
                config.netease.chatExtension = netease->value("chat_extension", false);
            }
            if (const auto mcp = root.find("mcp_server_config"); mcp != root.end() && mcp->is_object()) {
                config.mcpServer.enabled    = mcp->value("enabled", false);
                config.mcpServer.serverIp   = mcp->value("server_ip", "localhost");
                config.mcpServer.serverPort = mcp->value("server_port", 19133);
            }
            return config;
        }

        Json defaultConfigJson(const UserConfig& config) {
            return {
                {"included_mod_dirs", Json::array({"./"})},
                {"world_seed", nullptr},
                {"reset_world", config.world.reset},
                {"world_name", config.world.name},
                {"world_folder_name", config.world.folderName},
                {"world_source_path", "auto"},
                {"auto_join_game", config.world.autoJoin},
                {"include_debug_mod", config.includeDebugMod},
                {"log_protocol", static_cast<int>(config.logProtocol)},
                {"auto_hot_reload_mods", config.hotReload.mods},
                {"auto_hot_reload_ui", config.hotReload.ui},
                {"auto_hot_reload_shaders", config.hotReload.shaders},
                {"auto_hot_reload_materials", config.hotReload.materials},
                {"auto_hot_reload_particles", config.hotReload.particles},
                {"world_type", config.world.level.worldType},
                {"game_mode", config.world.level.gameMode},
                {"enable_cheats", config.world.level.enableCheats},
                {"keep_inventory", config.world.level.keepInventory},
                {"game_executable_path", MCDevTool::Utils::pathToGenericUtf8(config.gameExecutablePath)},
            };
        }

        Json readConfigJson(const std::filesystem::path& path) {
            std::ifstream     input(path, std::ios::binary);
            const std::string content{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            auto              result = Json::parse(content, nullptr, false, true);
            if (result.is_discarded()) {
                throw std::runtime_error("配置文件解析失败，JSON异常，请检查格式是否正确。");
            }
            return result;
        }
    } // namespace

    std::optional<std::filesystem::path> selectGameExePath(const std::vector<std::filesystem::path>& paths) {
        if (paths.empty()) {
            return std::nullopt;
        }
        if (getEnvIsSubprocessMode()) {
            return paths.front();
        }
        if (paths.size() == 1) {
            const auto version = MCDevTool::Utils::pathToUtf8(paths.front().parent_path().filename());
            std::cout << "Discovered game version: " << version << "  "
                      << MCDevTool::Utils::pathToGenericUtf8(paths.front()) << '\n';
            return paths.front();
        }

        std::cout << "Discovered game versions (newest first):\n";
        std::size_t versionWidth = 0;
        for (const auto& path : paths) {
            versionWidth = std::max(versionWidth, MCDevTool::Utils::pathToUtf8(path.parent_path().filename()).size());
        }
        for (std::size_t index = 0; index < paths.size(); ++index) {
            const auto version = MCDevTool::Utils::pathToUtf8(paths[index].parent_path().filename());
            std::cout << "  [" << index + 1 << "] " << version << std::string(versionWidth - version.size(), ' ')
                      << (index == 0 ? " [latest]" : "         ") << "  "
                      << MCDevTool::Utils::pathToGenericUtf8(paths[index]) << '\n';
        }

        while (true) {
            std::cout << "Select game version [1-" << paths.size() << "] (default 1): " << std::flush;
            std::string input;
            if (!std::getline(std::cin, input)) {
                std::cout << '\n';
                return paths.front();
            }
            const auto first = input.find_first_not_of(" \t");
            if (first == std::string::npos) {
                const auto version = MCDevTool::Utils::pathToUtf8(paths.front().parent_path().filename());
                std::cout << "Selected: " << version << " [latest]\n";
                return paths.front();
            }
            const auto last = input.find_last_not_of(" \t");
            input           = input.substr(first, last - first + 1);

            std::size_t selected    = 0;
            const auto [end, error] = std::from_chars(input.data(), input.data() + input.size(), selected);
            if (error == std::errc{} && end == input.data() + input.size() && selected >= 1
                && selected <= paths.size()) {
                const auto& selectedPath = paths[selected - 1];
                const auto  version      = MCDevTool::Utils::pathToUtf8(selectedPath.parent_path().filename());
                std::cout << "Selected: " << version << (selected == 1 ? " [latest]" : "") << '\n';
                return selectedPath;
            }
            std::cout << "Invalid selection. Enter a number from 1 to " << paths.size() << ".\n";
        }
    }

    UserConfig createDefaultConfig() {
        std::filesystem::path gameExecutablePath;
        if (auto discovered = selectGameExePath(MCDevTool::autoMatchGameExePaths())) {
            gameExecutablePath = std::move(*discovered);
        } else {
            std::string input;
            std::cout << "请输入游戏可执行文件路径：";
            std::getline(std::cin, input);
            if (input.size() > 2 && input.front() == '"' && input.back() == '"') {
                input = input.substr(1, input.size() - 2);
            }
            gameExecutablePath = std::filesystem::u8path(input);
        }
        if (!std::filesystem::is_regular_file(gameExecutablePath)) {
            std::cerr << "路径无效，文件不存在。\n";
            std::exit(1);
        }

        UserConfig config;
        config.gameExecutablePath = std::move(gameExecutablePath);
        config.modDirectories.emplace_back("./", true, true);
        return config;
    }

    UserConfig parseUserConfig(std::string_view jsonText) {
        auto root = Json::parse(jsonText, nullptr, false, true);
        if (root.is_discarded()) {
            throw std::runtime_error("配置文件解析失败，JSON异常，请检查格式是否正确。");
        }
        return parseUserConfigJson(root);
    }

    UserConfig userParseConfig() {
        const auto configPath = std::filesystem::current_path() / ".mcdev.json";
        if (!std::filesystem::is_regular_file(configPath)) {
            auto          config = createDefaultConfig();
            std::ofstream output(configPath, std::ios::binary);
            output << defaultConfigJson(config).dump(4);
            return config;
        }
        return parseUserConfigJson(readConfigJson(configPath));
    }

    bool updateGamePath(std::filesystem::path& path) {
        auto discovered = selectGameExePath(MCDevTool::autoMatchGameExePaths());
        if (!discovered) {
            return false;
        }
        path = std::move(*discovered);
        return true;
    }

    void tryUpdateUserGamePath(const std::filesystem::path& newPath) {
        const auto configPath          = std::filesystem::current_path() / ".mcdev.json";
        auto       config              = readConfigJson(configPath);
        config["game_executable_path"] = MCDevTool::Utils::pathToGenericUtf8(newPath);

        std::ofstream output(configPath, std::ios::binary | std::ios::trunc);
        output << config.dump(4);
    }

} // namespace mcdk
