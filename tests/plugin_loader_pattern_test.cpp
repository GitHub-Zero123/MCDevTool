// 加载器模式的验证：一个 DLL 充当其他插件的宿主。
// 这是 config 字段的真正用途（绑定 Python / Lua 等）。要成立需要两件事同时
#include <mcdk/plugin_host/host.hpp>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#ifndef MCDEV_TEST_LOADER_PLUGIN
#error "MCDEV_TEST_LOADER_PLUGIN must point at the built loader example"
#endif

namespace {

    std::vector<std::string> gOutput;

    bool expect(bool condition, const char* description) {
        if (!condition) {
            std::cerr << "Failed: " << description << '\n';
        }
        return condition;
    }

    bool sawLine(std::string_view needle) {
        return std::any_of(gOutput.begin(), gOutput.end(), [needle](const std::string& line) {
            return line.find(needle) != std::string::npos;
        });
    }

} // namespace

int main() {
    using namespace mcdk;

    const std::filesystem::path pluginPath = MCDEV_TEST_LOADER_PLUGIN;
    const auto                  path       = pluginPath.generic_string();
    bool                        passed     = true;

    plugin_host::Host host([](const std::string& message, ConsoleColor) { gOutput.push_back(message); });

    // 一个 DLL，三条声明，三个「脚本」。
    std::vector<PluginDeclaration> declarations{
        PluginDeclaration{
            .enabled    = true,
            .path       = path,
            .id         = "com.me.py.hud",
            .configJson = R"({"id":"com.me.py.hud","name":"HUD","script":"hud/main.py"})",
        },
        PluginDeclaration{
            .enabled    = true,
            .path       = path,
            .id         = "com.me.py.timer",
            .configJson = R"({"id":"com.me.py.timer","name":"Timer","script":"timer/main.py"})",
        },
        // 缺 script：该实例应当在 register 阶段失败并被卸载，其余两个不受影响。
        PluginDeclaration{
            .enabled    = true,
            .path       = path,
            .id         = "com.me.py.broken",
            .configJson = R"({"id":"com.me.py.broken","name":"Broken"})",
        },
    };
    host.loadDeclared(declarations, pluginPath.parent_path());

    // --- 动态身份 ----------------------------------------------------
    {
        const auto loaded  = host.loaded();
        passed            &= expect(loaded.size() == 3, "one DLL, three declarations, three instances");

        std::set<std::string> ids;
        for (const auto& plugin : loaded) {
            ids.insert(plugin.id);
        }
        passed &= expect(
            ids.size() == 3,
            "each instance reports its own id — without this they would all be the host DLL's compile-time id"
        );
        passed &= expect(ids.count("com.me.py.hud") == 1, "identity() overrides the id from config");
        passed &= expect(ids.count("com.me.py.timer") == 1, "identity() overrides the id from config");

        // version 在 identity() 里留空，必须回落到 MCDK_PLUGIN 的静态值。
        passed &= expect(
            std::all_of(
                loaded.begin(),
                loaded.end(),
                [](const plugin_host::LoadedPlugin& plugin) { return plugin.version == "0.1.0"; }
            ),
            "fields left empty by identity() fall back to the MCDK_PLUGIN literals"
        );
    }

    // --- 各实例持有自己的 config -------------------------------------
    host.advance(MCDK_STAGE_REGISTER);
    passed &= expect(sawLine("loader:register:hud/main.py"), "the first instance loaded its own script");
    passed &= expect(sawLine("loader:register:timer/main.py"), "the second instance loaded its own script");

    // 日志前缀用的是动态 id，用户才分得清是哪个脚本打的。
    passed &= expect(sawLine("[com.me.py.hud]"), "log output is tagged with the dynamic id");
    passed &= expect(sawLine("[com.me.py.timer]"), "log output is tagged with the dynamic id");

    // --- 一个实例失败不影响其余 ---------------------------------------
    {
        const auto loaded  = host.loaded();
        passed            &= expect(loaded.size() == 2, "the instance with no script was unloaded at register");
        passed            &= expect(
            std::none_of(
                loaded.begin(),
                loaded.end(),
                [](const plugin_host::LoadedPlugin& plugin) { return plugin.id == "com.me.py.broken"; }
            ),
            "the failed instance is the one that was removed"
        );
    }

    host.advance(MCDK_STAGE_RUNTIME);
    passed &= expect(sawLine("loader:runtime:hud/main.py"), "surviving instances keep receiving stages");
    passed &= expect(sawLine("loader:runtime:timer/main.py"), "surviving instances keep receiving stages");

    host.advance(MCDK_STAGE_SHUTDOWN);
    host.shutdown();
    passed &= expect(host.empty(), "every instance is retired");

    if (!passed) {
        std::cerr << "--- captured output ---\n";
        for (const auto& line : gOutput) {
            std::cerr << line << '\n';
        }
        std::cerr << "plugin_loader_pattern_test failed\n";
        return 1;
    }
    std::cout << "plugin_loader_pattern_test passed\n";
    return 0;
}
