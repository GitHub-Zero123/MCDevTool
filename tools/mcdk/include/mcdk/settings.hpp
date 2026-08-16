#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <mcdevtool/level.h>
#include <mcdevtool/style.h>

#include <mcdk/mod_dir_config.hpp>

namespace mcdk {

    enum class GameLogProtocol : std::uint8_t {
        Stdio = 0,
        Safaia = 1,
    };

    struct HotReloadConfig {
        bool mods      = true;
        bool ui        = false;
        bool shaders   = false;
        bool materials = false;
        bool particles = false;
    };

    struct WorldSourceConfig {
        enum class Mode {
            Auto,
            Disabled,
            Path,
        };

        Mode                  mode = Mode::Auto;
        std::filesystem::path path;
    };

    struct WorldProjectConfig {
        std::string                    name       = "MC_DEV_WORLD";
        std::string                    folderName = "MC_DEV_WORLD";
        WorldSourceConfig              source;
        bool                           reset    = false;
        bool                           autoJoin = true;
        MCDevTool::Level::LevelOptions level;
    };

    struct SkinConfig {
        bool                  slim = false;
        std::filesystem::path path;
    };

    struct PlayerConfig {
        std::string               name = "developer";
        std::optional<SkinConfig> skin;
    };

    struct ModPcDebuggerConfig {
        bool enabled = false;
        int  port    = 5632;
    };

    struct PtvsdConfig {
        bool        enabled = false;
        std::string ip      = "localhost";
        int         port    = 56788;
    };

    struct DebugModOptions {
        // debug_options is pass-through data for Python; keep one serialized form instead of rebuilding its JSON tree.
        std::string serializedJson = "{}";
    };

    struct NeteaseConfig {
        bool chatExtension = false;
    };

    struct McpServerConfig {
        bool        enabled    = false;
        std::string serverIp   = "localhost";
        int         serverPort = 19133;
    };

    struct UserConfig {
        std::filesystem::path         gameExecutablePath;
        std::vector<UserModDirConfig> modDirectories;
        WorldProjectConfig            world;
        PlayerConfig                  player;
        bool                          includeDebugMod = true;
        HotReloadConfig               hotReload;
        DebugModOptions               debugOptions;
        ModPcDebuggerConfig           modPcDebugger;
        PtvsdConfig                   ptvsdDebugger;
        MCDevTool::Style::StyleConfig windowStyle;
        NeteaseConfig                 netease;
        McpServerConfig               mcpServer;
        GameLogProtocol               logProtocol = GameLogProtocol::Stdio;
    };

} // namespace mcdk
