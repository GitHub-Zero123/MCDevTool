#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <mcdevtool/level.h>
#include <mcdevtool/style.h>

#include <mcdk/lane.hpp>
#include <mcdk/mod_dir_config.hpp>
#include <mcdk/plugin_declarations.hpp>

namespace mcdk {

    enum class GameLogProtocol : std::uint8_t {
        Stdio = 0,
        Safaia = 1,
    };

    // Addon 链接到哪里。World 让实例互不干扰，见 docs/cli.md。
    enum class PackLinkMode : std::uint8_t {
        Global = 0,
        World  = 1,
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
        // 监听失败时中止启动。来自 mcdk run，不读配置文件。
        bool        required   = false;
        // mc_input 默认走窗口消息投递。来自隔离会话，不读配置文件。
        bool        preferPostInput = false;
    };
    // 由 mcdk run 设置，不读配置文件。无参启动全部保持默认。
    struct AutomationConfig {
        std::string sessionName;
        // mcdk 退出时带走游戏，避免留下无人管理的进程。
        bool killGameWithMcdk = false;
        // 设置后游戏的 AppData 指向 laneRoot，见 mcdk/lane.hpp。
        std::optional<lane::Level> laneLevel;
        std::filesystem::path      laneRoot;  // 工程里的 Lane 目录
        std::filesystem::path      laneEntry; // 游戏实际使用的入口，见 lane::gameEntry
    };

    struct UserConfig {
        std::filesystem::path         gameExecutablePath;
        std::vector<UserModDirConfig> modDirectories;
        WorldProjectConfig            world;
        PlayerConfig                  player;
        bool                          includeDebugMod = true;
        PackLinkMode                  packLinkMode    = PackLinkMode::Global;
        HotReloadConfig               hotReload;
        DebugModOptions               debugOptions;
        ModPcDebuggerConfig           modPcDebugger;
        PtvsdConfig                   ptvsdDebugger;
        MCDevTool::Style::StyleConfig windowStyle;
        NeteaseConfig                 netease;
        McpServerConfig               mcpServer;
        AutomationConfig              automation;
        GameLogProtocol               logProtocol = GameLogProtocol::Stdio;
        std::vector<PluginDeclaration> plugins;
    };

} // namespace mcdk
