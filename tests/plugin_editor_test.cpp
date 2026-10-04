// 定制编辑器的两项能力：MCDEV_PLUGINS 注入插件、mcdk.world.resolve 改写存档。
// 插件经环境变量加载 examples/04-world，走完整的 C ABI。
#include <mcdk/config.hpp>
#include <mcdk/plugin_declarations.hpp>
#include <mcdk/plugin_host/host.hpp>
#include <mcdk/plugin_host/world_request.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#ifndef MCDEV_TEST_WORLD_PLUGIN
#error "MCDEV_TEST_WORLD_PLUGIN must point at the built example plugin"
#endif

namespace {

    bool gPassed = true;

    void expect(bool condition, const char* description) {
        if (!condition) {
            std::cerr << "Failed: " << description << '\n';
            gPassed = false;
        }
    }

    bool contains(const std::vector<std::string>& lines, std::string_view needle) {
        for (const auto& line : lines) {
            if (line.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    void setEnv(const std::string& value) {
#ifdef _WIN32
        _putenv_s("MCDEV_PLUGINS", value.c_str());
#else
        setenv("MCDEV_PLUGINS", value.c_str(), 1);
#endif
    }

    // 经 MCDEV_PLUGINS 加载 04-world，让它对 current 做一次改写。
    struct Run {
        std::vector<std::string>                       output;
        std::optional<mcdk::plugin_host::WorldOverride> result;
        bool                                           threw = false;
    };

    Run runWorldPlugin(const std::string& pluginConfig, const mcdk::WorldProjectConfig& current) {
        const std::filesystem::path plugin = MCDEV_TEST_WORLD_PLUGIN;
        setEnv(R"([{"enable":true,"path":")" + plugin.generic_string() + R"(","config":)" + pluginConfig + "}]");

        Run                     run;
        mcdk::plugin_host::Host host([&run](const std::string& message, mcdk::ConsoleColor) {
            run.output.push_back(message);
        });
        host.loadDeclared(mcdk::plugin_host::detail::parseEnvPluginDeclarations(), plugin.parent_path());
        expect(host.loaded().size() == 1, "the plugin declared in MCDEV_PLUGINS is loaded");
        host.advance(MCDK_STAGE_REGISTER);
        host.advance(MCDK_STAGE_CONFIG);
        try {
            run.result = mcdk::plugin_host::resolveWorld(current);
        } catch (const std::runtime_error&) {
            run.threw = true;
        }
        host.shutdown();
        setEnv("");
        return run;
    }

} // namespace

int main() {
    using mcdk::WorldProjectConfig;

    // --- MCDEV_PLUGINS 的解析 -----------------------------------------
    setEnv("");
    expect(mcdk::plugin_host::detail::parseEnvPluginDeclarations().empty(), "unset MCDEV_PLUGINS yields nothing");
    setEnv("not json");
    try {
        (void)mcdk::plugin_host::detail::parseEnvPluginDeclarations();
        expect(false, "malformed MCDEV_PLUGINS is rejected loudly");
    } catch (const std::runtime_error&) {
    }
    setEnv(R"([{"enable":true,"path":"a","priority":5},{"enable":false,"path":"b","id":"x.y","config":{"k":1}}])");
    {
        const auto parsed = mcdk::plugin_host::detail::parseEnvPluginDeclarations();
        expect(parsed.size() == 2, "both declarations parsed");
        if (parsed.size() == 2) {
            // 按 priority 稳定排序：b(0) 在 a(5) 前。
            expect(parsed[0].path == "b" && parsed[0].id == "x.y" && parsed[0].configJson == R"({"k":1})", "fields");
            expect(parsed[1].path == "a" && parsed[1].enabled && parsed[1].priority == 5, "priority ordering");
        }
    }
    setEnv("");

    // --- 存档键的解析：.mcdev.json 与插件改写共用同一份 -----------------
    {
        const auto defaults = mcdk::parseUserConfig("{}");
        expect(
            mcdk::worldConfigToJson(defaults.world) == mcdk::worldConfigToJson(WorldProjectConfig{}),
            "an empty .mcdev.json still yields the default world"
        );
        const auto custom = mcdk::parseUserConfig(R"({
            "world_name":"W","world_folder_name":"F","reset_world":true,"auto_join_game":false,
            "world_seed":42,"world_type":2,"game_mode":0,"enable_cheats":false,"keep_inventory":false,
            "do_weather_cycle":false,"do_daylight_cycle":false,"world_source_path":null,
            "experiment_options":{"gametest":true}
        })");
        WorldProjectConfig roundTrip;
        mcdk::applyWorldConfig(mcdk::worldConfigToJson(custom.world), roundTrip);
        expect(
            mcdk::worldConfigToJson(roundTrip) == mcdk::worldConfigToJson(custom.world),
            "every world key survives a JSON round trip"
        );
        expect(custom.world.level.seed == 42u && custom.world.level.experimentsOptions.gametest, "nested keys parsed");
        expect(!mcdk::isWorldConfigKey("game_executable_path"), "non-world keys are not world keys");
    }

    // --- 目录名校验 ---------------------------------------------------
    expect(mcdk::plugin_host::isValidWorldFolderName("EDITOR_WORLD"), "plain name accepted");
    expect(mcdk::plugin_host::isValidWorldFolderName("编辑器存档"), "non-ASCII name accepted");
    for (const auto* bad : {"", ".", "..", "../x", "a/b", "a\\b", "C:", "x.", "x "}) {
        expect(!mcdk::plugin_host::isValidWorldFolderName(bad), "unsafe folder name rejected");
    }

    WorldProjectConfig current;
    current.name             = "MyWorld";
    current.folderName       = "MyFolder";
    current.level.gameMode   = 0;
    current.level.worldType  = 2;

    // --- 按键覆盖：只动给出的键 ----------------------------------------
    {
        const auto run = runWorldPlugin(R"({"folder":"EDITOR_WORLD","reset":"true","mode":"merge"})", current);
        expect(contains(run.output, "world:current:MyFolder"), "the plugin sees the current settings");
        expect(run.result.has_value(), "merge override reaches the host");
        if (run.result) {
            expect(run.result->ownerId == "com.example.editor-world", "the override names its plugin");
            expect(!run.result->replaced, "merge is reported as merge");
            expect(run.result->world.folderName == "EDITOR_WORLD" && run.result->world.reset, "given keys applied");
            expect(
                run.result->world.level.gameMode == 0 && run.result->world.level.worldType == 2,
                "keys not given keep the user's values under merge"
            );
        }
    }

    // --- 完全覆盖：没给的键回到默认值 -----------------------------------
    {
        const auto run = runWorldPlugin(R"({"folder":"EDITOR_WORLD","mode":"replace"})", current);
        expect(run.result.has_value(), "replace override reaches the host");
        if (run.result) {
            expect(run.result->replaced, "replace is reported as replace");
            expect(run.result->world.folderName == "EDITOR_WORLD", "given keys applied");
            expect(
                run.result->world.level.gameMode == WorldProjectConfig{}.level.gameMode
                    && run.result->world.level.worldType == WorldProjectConfig{}.level.worldType,
                "keys not given fall back to defaults under replace"
            );
        }
    }

    // --- 非法目录名：插件拿到错误、否决、启动中止 ------------------------
    {
        const auto run = runWorldPlugin(R"({"folder":"../escape","mode":"merge"})", current);
        expect(
            contains(run.output, "world:override:" + std::to_string(MCDK_ERR_INVALID_ARGUMENT)),
            "an escaping folder name is rejected with INVALID_ARGUMENT"
        );
        expect(contains(run.output, "world:error:world_folder_name"), "the reason reaches the plugin");
        expect(run.threw && !run.result, "the plugin's veto aborts the launch");
    }

    // --- 没有插件：不改写 ----------------------------------------------
    expect(!mcdk::plugin_host::resolveWorld(current).has_value(), "no subscribers, no override");

    std::cout << (gPassed ? "plugin_editor_test passed\n" : "plugin_editor_test failed\n");
    return gPassed ? 0 : 1;
}
