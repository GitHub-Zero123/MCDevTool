// mcdk_core: 强类型启动配置 LaunchConfig
//
// 取代原先的裸 nlohmann::json 入参。这是为核心模式重新布局的配置模型——
// 不再继承磁盘 .mcdev.json"所有字段堆在根级"的历史包袱，而是按语义分组成嵌套子结构：
//   world（世界）/ player（玩家）/ hotReload（热更新）/ debug（调试），
//   外加 mcpServer / netease / windowStyle 等自成一体的服务/外观块。
//
// 设计（v2 决策）：
//   - 所有有固定 schema 的配置块一律强类型，业务方点字段即可配置，不需要知道任何 json 形状。
//   - 唯一例外：debug.options——它被原样 dump 注入调试 mod 的 Config.py（true→True/false→False/
//     null→None），其键集由 python mod 自己定义、可独立演进，mcdk 不拥有该 schema，
//     因此这一块（且仅这一块）保留 nlohmann::json 作为"自由透传"。
//   - fromJson/toJson 仅作与磁盘 .mcdev.json（扁平结构）的兼容适配层（CLI 走这条路）：
//     负责"扁平 json ↔ 嵌套结构体"的双向映射，磁盘格式不受结构调整影响。
//
// 关键约束：每个字段默认值必须逐字段 == 原 helper 的 config.value(key, default) 默认值，
// 这样核心模式（直接构造 LaunchConfig）与 CLI 模式（fromJson）行为完全一致。
//
// 本头刻意只依赖 <nlohmann/json.hpp> + 标准库，不拖入 mcdevtool/mcp 等重头，
// 便于业务方仅 include 本头即可声明配置。
//
// By Zero123
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace mcdk::core {

    // 强类型启动配置。字段按语义分组；与磁盘 .mcdev.json 的扁平 key 映射见 fromJson/toJson。
    struct LaunchConfig {
        // ===================== 叶子类型 =====================

        // 实验性玩法（experiment_options）。
        struct Experiments {
            bool dataDrivenBiomes           = false;  // data_driven_biomes
            bool dataDrivenItems            = false;  // data_driven_items
            bool experimentalMolangFeatures = false;  // experimental_molang_features
        };

        // 单个 mod 目录（included_mod_dirs 的元素）。
        struct ModDir {
            std::string path      = "./";  // u8 路径
            bool        hotReload = true;  // hot_reload
            bool        enabled   = true;  // enabled
        };

        // 玩家皮肤（skin_info）。skin 留空时启动逻辑回填默认 vanilla steve。
        struct SkinInfo {
            bool        slim = false;  // slim
            std::string skin;          // skin（皮肤文件路径）
        };

        // MCP 服务器（mcp_server_config）。
        struct McpServer {
            bool        enabled    = false;
            std::string serverIp   = "localhost";  // server_ip
            int         serverPort = 19133;        // server_port
        };

        // 官方 ptvsd 调试器（ptvsd_debugger）。环境变量 MCDEV_PTVSD_IP/PORT 优先（启动时解析）。
        struct PtvsdDebugger {
            bool        enabled = false;        // enabled
            std::string ip      = "localhost";  // ip
            int         port    = 56788;        // port
        };

        // 历史 mcdbg 调试器附加（modpc_debugger）。环境变量 MCDEV_MODPC_DEBUGGER_PORT 优先。
        struct ModpcDebugger {
            bool enabled = false;  // enabled
            int  port    = 5632;   // port
        };

        // 网易客户端开关（netease_config）。目前仅 chat_extension。
        struct NeteaseConfig {
            bool chatExtension = false;  // chat_extension
        };

        // 游戏窗口样式（window_style）。各 optional 子项"是否提供"决定是否真正改样式。
        struct WindowStyle {
            bool alwaysOnTop  = false;  // always_on_top
            bool hideTitleBar = false;  // hide_title_bar
            std::optional<std::array<std::uint8_t, 3>> titleBarColor;  // title_bar_color [r,g,b]
            std::optional<std::array<int, 2>>          fixedSize;      // fixed_size [w,h]
            std::optional<std::array<int, 2>>          fixedPosition;  // fixed_position [x,y]
            std::optional<int>                         lockCorner;     // lock_corner
        };

        // ===================== 分组 =====================

        // 世界生成与规则。
        struct World {
            std::string                  name           = "MC_DEV_WORLD";  // world_name（写入 level.dat）
            std::string                  folderName     = "MC_DEV_WORLD";  // world_folder_name（存档目录名）
            std::optional<std::uint64_t> seed;                             // world_seed（空 = 随机）
            bool                         reset          = false;           // reset_world
            int                          type           = 1;              // world_type 0-旧版 1-无限 2-平坦
            int                          gameMode       = 1;              // game_mode  0-生存 1-创造 2-冒险
            bool                         enableCheats   = true;           // enable_cheats
            bool                         keepInventory  = true;           // keep_inventory
            bool                         doWeatherCycle = true;           // do_weather_cycle
            bool                         doDaylightCycle = true;          // do_daylight_cycle
            std::optional<Experiments>   experiments;                      // experiment_options（存在性即语义）
        };

        // 进档玩家身份。
        struct Player {
            std::string             name = "developer";  // user_name
            std::optional<SkinInfo> skin;                // skin_info（存在性即语义）
        };

        // 热更新 watcher 开关。
        struct HotReload {
            bool mods      = true;   // auto_hot_reload_mods
            bool ui        = false;  // auto_hot_reload_ui
            bool shaders   = false;  // auto_hot_reload_shaders
            bool materials = false;  // auto_hot_reload_materials
        };

        // 调试相关。
        struct Debug {
            bool          includeDebugMod = true;  // include_debug_mod（注入 R 键热更新 + py 输出流标记）
            PtvsdDebugger ptvsd;                    // ptvsd_debugger
            ModpcDebugger modpc;                    // modpc_debugger
            // debug_options：唯一自由透传块。键集由调试 mod 的 Config.py 定义，mcdk 不拥有 schema，
            // 原样 dump 注入。缺省为 null（消费侧视作空 {}）。
            nlohmann::json options;
        };

        // ===================== 成员 =====================

        // 游戏可执行文件路径（u8 字符串）。无默认——核心模式必填；无效时由 LaunchOptions 的
        // policy/prompter 决定行为（见 launcher.h）。
        std::string gameExecutablePath;

        bool autoJoinGame = true;  // auto_join_game：是否自动进入存档

        World     world;                            // 世界
        Player    player;                           // 玩家
        std::vector<ModDir> includedModDirs{ ModDir{} };  // included_mod_dirs（缺省 = 单个 "./"）
        HotReload hotReload;                        // 热更新
        Debug     debug;                            // 调试
        McpServer mcpServer;                        // mcp_server_config
        NeteaseConfig netease;                      // netease_config
        WindowStyle   windowStyle;                  // window_style

        // ===================== 与磁盘 .mcdev.json（扁平结构）的互转（适配层）=====================

        // 解析诊断：结构化、机器可判定，业务方可 switch(code) 特化处理，无需解析文案。
        struct Diagnostic {
            // 诊断类别（可扩展）。业务方据此分支处理。
            enum class Code {
                InvalidModDirElement, // included_mod_dirs 元素既非字符串路径、也非对象
            };

            Code        code;     // 机器可判定的类别
            std::string pointer;  // 出错位置（RFC6901 JSON Pointer，如 "/included_mod_dirs/1"，可喂 nlohmann::json::json_pointer 定位）
            std::string detail;   // 补充信息（如实际遇到的 json 类型名），供展示/排查，非用于程序判定

            // 便利：渲染一条默认人类可读消息。业务方完全可无视，自行根据 code/pointer 处理。
            std::string message() const;
        };

        // 从扁平 .mcdev.json 风格的 json 构造嵌套强类型配置。
        // 缺失字段一律取上面声明的默认值（与原 helper 的 config.value(key,default) 一致）。
        //
        // 错误处理（现代化、非抛异常）：对可容忍的局部异常（如 included_mod_dirs 含既非字符串、
        // 又非对象的非法元素）不 throw、也不静默吞掉，而是**跳过该项并把一条结构化 Diagnostic**
        // 追加到 diagnostics（传 nullptr 则跳过但不记录）。调用方据 Diagnostic.code 程序化处理。
        // 注：字段类型彻底写错（如 world_seed:"abc"）这类无法容忍的错误仍由底层 nlohmann 抛出。
        static LaunchConfig fromJson(const nlohmann::json& j, std::vector<Diagnostic>* diagnostics = nullptr);

        // 反向序列化为扁平 .mcdev.json 风格的 json。
        // 固定 schema 块稠密输出（总是带上，自带文档性）；optional/自由透传块仅在有值时输出，
        // 因此对"全字段配置"可逐字段往返（round-trip）。
        nlohmann::json toJson() const;
    };

} // namespace mcdk::core
