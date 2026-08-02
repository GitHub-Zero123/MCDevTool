#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <mcdevtool/level.h>
#include <mcdevtool/style.h>

#include "mod_dir_config.hpp"

namespace mcdk {

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

    struct DebugOptionValue {
        using Array  = std::vector<DebugOptionValue>;
        using Object = std::map<std::string, DebugOptionValue>;
        using Storage =
            std::variant<std::nullptr_t, bool, int64_t, uint64_t, double, std::string, Array, Object>;

        Storage value = nullptr;
    };

    struct DebugModOptions {
        std::optional<DebugOptionValue> reloadKey;
        std::optional<DebugOptionValue> reloadWorldKey;
        std::optional<DebugOptionValue> reloadAddonKey;
        std::optional<DebugOptionValue> reloadShadersKey;
        std::optional<DebugOptionValue> reloadKeyGlobal;
        DebugOptionValue::Object        additionalOptions;
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
    };

} // namespace mcdk
