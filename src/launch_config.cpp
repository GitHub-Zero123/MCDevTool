// mcdk_core: LaunchConfig 与 .mcdev.json 的互转适配层实现
//
// 这是"嵌套强类型 LaunchConfig"与"磁盘 .mcdev.json 扁平裸 json"之间的唯一桥。
// 仅 CLI 兼容路径（main.cpp）与 smoke test 往返校验会用到；核心模式直接构造 LaunchConfig，
// 不经过本文件。结构体分组与磁盘扁平 key 的映射全部集中在这里。
//
// 不变量：fromJson 的每个 value(key, default) 默认值，必须与 LaunchConfig 头里声明的成员默认值、
// 以及原各 helper 的 config.value(key, default) 默认值三者完全一致。
//
// By Zero123
#include <mcdk_core/launch_config.h>

namespace mcdk::core {

    namespace {
        // 读取定长整数数组的前 N 个元素（沿用原 window_style 解析：要求 size>=N，只取前 N，容忍多余）。
        template <typename T, std::size_t N>
        std::optional<std::array<T, N>> readFixedArray(const nlohmann::json& parent, const char* key) {
            if (!parent.contains(key) || !parent[key].is_array() || parent[key].size() < N) {
                return std::nullopt;
            }
            const auto&      arr = parent[key];
            std::array<T, N> out{};
            for (std::size_t i = 0; i < N; ++i) {
                out[i] = arr[i].get<T>();
            }
            return out;
        }

        // 解析单个 included_mod_dirs 元素（沿用原 parseFromJson 语义）。
        // 现代化错误处理：非法元素（既非字符串路径、又非对象）返回 std::nullopt 而非抛异常，
        // 由调用方决定跳过 + 记录诊断。
        std::optional<LaunchConfig::ModDir> parseModDir(const nlohmann::json& item) {
            LaunchConfig::ModDir d;
            if (item.is_string()) {
                d.path      = item.get<std::string>();
                d.hotReload = true; // 字符串形式：原逻辑强制 hotReload=true、enabled=true
                d.enabled   = true;
                return d;
            }
            if (item.is_object()) {
                d.path      = item.value("path", std::string("./"));
                d.hotReload = item.value("hot_reload", true);
                d.enabled   = item.value("enabled", true);
                return d;
            }
            return std::nullopt;
        }
    } // namespace

    std::string LaunchConfig::Diagnostic::message() const {
        switch (code) {
        case Code::InvalidModDirElement:
            return "included_mod_dirs " + pointer + " 元素类型非法（应为字符串路径或 {path,hot_reload,enabled} 对象"
                   + (detail.empty() ? std::string() : "，实际为 " + detail) + "），已跳过";
        }
        return "未知诊断 " + pointer;
    }

    LaunchConfig LaunchConfig::fromJson(const nlohmann::json& j, std::vector<Diagnostic>* diagnostics) {
        LaunchConfig c;
        if (!j.is_object()) {
            // 非对象（如空 config / null）视作全默认，等价于原版"字段缺失走默认"。
            return c;
        }

        // ---------- 顶层 ----------
        c.gameExecutablePath = j.value("game_executable_path", c.gameExecutablePath);
        c.autoJoinGame       = j.value("auto_join_game", c.autoJoinGame);

        // ---------- world ----------
        if (j.contains("world_seed") && !j["world_seed"].is_null()) {
            c.world.seed = j["world_seed"].get<std::uint64_t>();
        }
        c.world.reset           = j.value("reset_world", c.world.reset);
        c.world.name            = j.value("world_name", c.world.name);
        c.world.folderName      = j.value("world_folder_name", c.world.folderName);
        c.world.type            = j.value("world_type", c.world.type);
        c.world.gameMode        = j.value("game_mode", c.world.gameMode);
        c.world.enableCheats    = j.value("enable_cheats", c.world.enableCheats);
        c.world.keepInventory   = j.value("keep_inventory", c.world.keepInventory);
        c.world.doWeatherCycle  = j.value("do_weather_cycle", c.world.doWeatherCycle);
        c.world.doDaylightCycle = j.value("do_daylight_cycle", c.world.doDaylightCycle);
        // experiment_options：存在性即语义（存在对象 → 启用实验性玩法）
        if (j.contains("experiment_options") && j["experiment_options"].is_object()) {
            const auto& e = j["experiment_options"];
            Experiments exp;
            exp.dataDrivenBiomes           = e.value("data_driven_biomes", false);
            exp.dataDrivenItems            = e.value("data_driven_items", false);
            exp.experimentalMolangFeatures = e.value("experimental_molang_features", false);
            c.world.experiments            = exp;
        }

        // ---------- player ----------
        c.player.name = j.value("user_name", c.player.name);
        // skin_info：存在性即语义（存在对象 → 用户自定义皮肤分支）
        if (j.contains("skin_info") && j["skin_info"].is_object()) {
            const auto& s = j["skin_info"];
            SkinInfo    sk;
            sk.slim      = s.value("slim", false);
            sk.skin      = s.value("skin", std::string());
            c.player.skin = sk;
        }

        // ---------- included_mod_dirs（支持 string / object 元素，沿用原 parseFromJson 语义）----------
        // 现代化错误处理：非法元素不抛异常，而是跳过 + 向 diagnostics 追加一条结构化诊断。
        if (j.contains("included_mod_dirs") && j["included_mod_dirs"].is_array()) {
            c.includedModDirs.clear();
            const auto& arr = j["included_mod_dirs"];
            for (std::size_t i = 0; i < arr.size(); ++i) {
                if (auto d = parseModDir(arr[i])) {
                    c.includedModDirs.push_back(std::move(*d));
                } else if (diagnostics) {
                    diagnostics->push_back(Diagnostic{
                        Diagnostic::Code::InvalidModDirElement,
                        "/included_mod_dirs/" + std::to_string(i),
                        arr[i].type_name(),
                    });
                }
            }
        }

        // ---------- hotReload ----------
        c.hotReload.mods      = j.value("auto_hot_reload_mods", c.hotReload.mods);
        c.hotReload.ui        = j.value("auto_hot_reload_ui", c.hotReload.ui);
        c.hotReload.shaders   = j.value("auto_hot_reload_shaders", c.hotReload.shaders);
        c.hotReload.materials = j.value("auto_hot_reload_materials", c.hotReload.materials);

        // ---------- debug ----------
        c.debug.includeDebugMod = j.value("include_debug_mod", c.debug.includeDebugMod);
        if (j.contains("ptvsd_debugger") && j["ptvsd_debugger"].is_object()) {
            const auto& p         = j["ptvsd_debugger"];
            c.debug.ptvsd.enabled = p.value("enabled", false);
            c.debug.ptvsd.ip      = p.value("ip", std::string("localhost"));
            c.debug.ptvsd.port    = p.value("port", 56788);
        }
        if (j.contains("modpc_debugger") && j["modpc_debugger"].is_object()) {
            const auto& m         = j["modpc_debugger"];
            c.debug.modpc.enabled = m.value("enabled", false);
            c.debug.modpc.port    = m.value("port", 5632);
        }
        if (j.contains("debug_options")) {
            c.debug.options = j["debug_options"];
        }

        // ---------- mcp_server_config ----------
        if (j.contains("mcp_server_config") && j["mcp_server_config"].is_object()) {
            const auto& m          = j["mcp_server_config"];
            c.mcpServer.enabled    = m.value("enabled", false);
            c.mcpServer.serverIp   = m.value("server_ip", std::string("localhost"));
            c.mcpServer.serverPort = m.value("server_port", 19133);
        }

        // ---------- netease_config ----------
        if (j.contains("netease_config") && j["netease_config"].is_object()) {
            c.netease.chatExtension = j["netease_config"].value("chat_extension", false);
        }

        // ---------- window_style ----------
        if (j.contains("window_style") && j["window_style"].is_object()) {
            const auto& w               = j["window_style"];
            c.windowStyle.alwaysOnTop   = w.value("always_on_top", false);
            c.windowStyle.hideTitleBar  = w.value("hide_title_bar", false);
            c.windowStyle.titleBarColor = readFixedArray<std::uint8_t, 3>(w, "title_bar_color");
            c.windowStyle.fixedSize     = readFixedArray<int, 2>(w, "fixed_size");
            c.windowStyle.fixedPosition = readFixedArray<int, 2>(w, "fixed_position");
            if (w.contains("lock_corner") && w["lock_corner"].is_number_integer()) {
                c.windowStyle.lockCorner = w["lock_corner"].get<int>();
            }
        }

        return c;
    }

    nlohmann::json LaunchConfig::toJson() const {
        nlohmann::json j = nlohmann::json::object();

        // 顶层
        j["game_executable_path"] = gameExecutablePath;
        j["auto_join_game"]       = autoJoinGame;

        // world（标量稠密输出；seed/experiments 按存在性）
        j["world_seed"]        = world.seed.has_value() ? nlohmann::json(*world.seed) : nlohmann::json(nullptr);
        j["reset_world"]       = world.reset;
        j["world_name"]        = world.name;
        j["world_folder_name"] = world.folderName;
        j["world_type"]        = world.type;
        j["game_mode"]         = world.gameMode;
        j["enable_cheats"]     = world.enableCheats;
        j["keep_inventory"]    = world.keepInventory;
        j["do_weather_cycle"]  = world.doWeatherCycle;
        j["do_daylight_cycle"] = world.doDaylightCycle;
        if (world.experiments.has_value()) {
            j["experiment_options"] = nlohmann::json{
                {"data_driven_biomes", world.experiments->dataDrivenBiomes},
                {"data_driven_items", world.experiments->dataDrivenItems},
                {"experimental_molang_features", world.experiments->experimentalMolangFeatures},
            };
        }

        // player
        j["user_name"] = player.name;
        if (player.skin.has_value()) {
            j["skin_info"] = nlohmann::json{
                {"slim", player.skin->slim},
                {"skin", player.skin->skin},
            };
        }

        // included_mod_dirs：全默认(hotReload && enabled)的项回写为字符串（与默认配置 ["./"] 一致），
        // 否则回写为对象，保证含 enabled/hot_reload 覆写的项可往返。
        nlohmann::json dirs = nlohmann::json::array();
        for (const auto& d : includedModDirs) {
            if (d.hotReload && d.enabled) {
                dirs.push_back(d.path);
            } else {
                dirs.push_back(nlohmann::json{
                    {"path", d.path},
                    {"hot_reload", d.hotReload},
                    {"enabled", d.enabled},
                });
            }
        }
        j["included_mod_dirs"] = std::move(dirs);

        // hotReload
        j["auto_hot_reload_mods"]      = hotReload.mods;
        j["auto_hot_reload_ui"]        = hotReload.ui;
        j["auto_hot_reload_shaders"]   = hotReload.shaders;
        j["auto_hot_reload_materials"] = hotReload.materials;

        // debug
        j["include_debug_mod"] = debug.includeDebugMod;
        j["ptvsd_debugger"]    = nlohmann::json{
            {"enabled", debug.ptvsd.enabled},
            {"ip", debug.ptvsd.ip},
            {"port", debug.ptvsd.port},
        };
        j["modpc_debugger"] = nlohmann::json{
            {"enabled", debug.modpc.enabled},
            {"port", debug.modpc.port},
        };
        if (!debug.options.is_null()) {
            j["debug_options"] = debug.options;
        }

        // mcp_server_config（固定 schema，稠密输出，自带文档性）
        j["mcp_server_config"] = nlohmann::json{
            {"enabled", mcpServer.enabled},
            {"server_ip", mcpServer.serverIp},
            {"server_port", mcpServer.serverPort},
        };

        // netease_config
        j["netease_config"] = nlohmann::json{
            {"chat_extension", netease.chatExtension},
        };

        // window_style：两个 bool 稠密；四个 optional 子项仅在有值时带上
        {
            nlohmann::json ws = nlohmann::json{
                {"always_on_top", windowStyle.alwaysOnTop},
                {"hide_title_bar", windowStyle.hideTitleBar},
            };
            if (windowStyle.titleBarColor.has_value()) {
                ws["title_bar_color"] = *windowStyle.titleBarColor;
            }
            if (windowStyle.fixedSize.has_value()) {
                ws["fixed_size"] = *windowStyle.fixedSize;
            }
            if (windowStyle.fixedPosition.has_value()) {
                ws["fixed_position"] = *windowStyle.fixedPosition;
            }
            if (windowStyle.lockCorner.has_value()) {
                ws["lock_corner"] = *windowStyle.lockCorner;
            }
            j["window_style"] = std::move(ws);
        }

        return j;
    }

} // namespace mcdk::core
