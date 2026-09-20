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

    // .mcdev.json 的 plugins 数组中的一条。
    //
    // 宿主禁止扫描任何目录来发现插件：插件必须由用户在此显式声明，未声明的
    // 动态库一律不加载。信任决策因此由用户做出且可审计。
    // 见 docs/plugin-system/06-loading.md §1。
    struct PluginDeclaration {
        bool enabled = false;
        // 原样保留。相对路径由加载器按 .mcdev.json 所在目录解析，而非进程工作目录。
        std::string path;
        // 期望的插件 id。非空时与插件自报的 id 比对，不符则拒绝加载（防替换）。
        std::string id;
        // 传给该插件的配置，任意 JSON。序列化后原样透传，插件通过
        // mcdk.core 的 get_config 取回文本自行解析。
        // 这是「同一个插件二进制按不同参数声明多次」的基础。
        // 字段缺省时为字面量 "null"，使插件永远可以直接 parse，无需特判。
        std::string configJson = "null";
        // 覆盖默认的声明顺序，小者先加载。
        int priority = 0;
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
        std::vector<PluginDeclaration> plugins;
    };

} // namespace mcdk
