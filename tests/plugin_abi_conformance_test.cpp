//
// ABI 一致性套件（docs/plugin-system/09-compatibility.md §2）。
//
// 加载 examples/00-abi-conformance 构建出的真实 DLL，逐条验证异常屏障的行为
// 与 02-abi-contract.md §4.2 的失败语义表一致。
//
// 同一个 DLL 被声明多次、各带不同 config，因此本文件同时也是「可传参式插件
// 必须是多实例」的回归测试。
//
#include <mcdk/plugin_host/guard.hpp>
#include <mcdk/plugin_host/host.hpp>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef MCDEV_TEST_CONFORMANCE_PLUGIN
#error "MCDEV_TEST_CONFORMANCE_PLUGIN must point at the built conformance plugin"
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

    std::size_t countLines(std::string_view needle) {
        return static_cast<std::size_t>(
            std::count_if(gOutput.begin(), gOutput.end(), [needle](const std::string& line) {
                return line.find(needle) != std::string::npos;
            })
        );
    }

    mcdk::PluginDeclaration declare(const std::string& path, const std::string& config) {
        return mcdk::PluginDeclaration{
            .enabled    = true,
            .path       = path,
            .id         = "com.mcdev.abi-conformance",
            .configJson = config,
            .priority   = 0,
        };
    }

} // namespace

int main() {
    using namespace mcdk;

    const std::filesystem::path pluginPath = MCDEV_TEST_CONFORMANCE_PLUGIN;
    const auto                  path       = pluginPath.generic_string();
    bool                        passed     = true;

    // ------------------------------------------------------------------
    // 一、宿主侧屏障（方向：宿主 → 插件）
    //
    // 宿主现有代码大量使用异常，这些异常绝不能穿过 C 边界。这里直接测
    // host::guard 本身，不经过 ABI —— 它是宿主侧代码，可以直接调。
    // ------------------------------------------------------------------
    {
        const auto status = plugin_host::guard([]() -> mcdk_status { throw std::runtime_error("boom"); });
        passed &= expect(status == MCDK_ERR_HOST_EXCEPTION, "host::guard converts std::exception to a status");
        passed &= expect(
            plugin_host::errorSlot().message.find("boom") != std::string::npos,
            "host::guard records the exception message in the error slot"
        );
    }
    {
        const auto status  = plugin_host::guard([]() -> mcdk_status { throw 42; });
        passed            &= expect(status == MCDK_ERR_HOST_EXCEPTION, "host::guard catches non-std::exception throws");
    }
    {
        const auto status  = plugin_host::guard([]() -> mcdk_status { return MCDK_OK; });
        passed            &= expect(status == MCDK_OK, "host::guard passes success through unchanged");
    }

    // ------------------------------------------------------------------
    // 二、插件侧屏障（方向：插件 → 宿主）
    // ------------------------------------------------------------------
    plugin_host::Host host([](const std::string& message, ConsoleColor) { gOutput.push_back(message); });

    std::vector<PluginDeclaration> declarations{
        declare(path, R"({"tag":"ok"})"),
        declare(path, R"({"tag":"reg","throw_at":"register","throw_kind":"std"})"),
        declare(path, R"({"tag":"run","throw_at":"runtime","throw_kind":"int"})"),
        declare(path, R"({"tag":"cfg","throw_at":"config","throw_kind":"custom"})"),
        declare(path, R"({"tag":"unl","throw_at":"shutdown","throw_kind":"std"})"),
    };
    host.loadDeclared(declarations, pluginPath.parent_path());

    passed &= expect(host.loaded().size() == 5, "the same DLL can be declared five times");

    // --- 多实例：每个实例必须看到自己的 config -------------------------
    host.advance(MCDK_STAGE_REGISTER);
    for (const char* tag : {"ok", "reg", "run", "cfg", "unl"}) {
        passed &= expect(
            sawLine(std::string("conformance:") + tag + ":register"),
            "each declaration gets its own Context and config"
        );
    }

    // --- REGISTER 失败 = 加载失败，该插件被卸载 -------------------------
    {
        const auto loaded  = host.loaded();
        passed            &= expect(loaded.size() == 4, "a plugin that throws at register is unloaded");
        passed            &= expect(
            std::none_of(
                loaded.begin(),
                loaded.end(),
                [](const plugin_host::LoadedPlugin& plugin) { return plugin.degraded; }
            ),
            "unloading the failed plugin leaves no degraded entry behind"
        );
    }

    // --- 非 REGISTER 阶段失败 = 降级，其余插件不受影响 -------------------
    host.advance(MCDK_STAGE_CONFIG);
    passed &= expect(sawLine("conformance:ok:config"), "a healthy plugin still receives later stages");
    {
        // loaded() 按值返回：必须先绑定到具名对象，否则 begin()/end() 来自
        // 两个不同的临时 vector，是悬垂迭代器。
        const auto loaded   = host.loaded();
        const auto degraded = std::count_if(loaded.begin(), loaded.end(), [](const plugin_host::LoadedPlugin& plugin) {
            return plugin.degraded;
        });
        passed &= expect(degraded == 1, "throwing a custom (non-std) type at config degrades exactly one plugin");
    }

    const auto worldBefore = countLines("conformance:cfg:");
    host.advance(MCDK_STAGE_WORLD);
    passed &= expect(countLines("conformance:cfg:") == worldBefore, "a degraded plugin receives no further stages");
    passed &= expect(sawLine("conformance:ok:world"), "degrading one plugin does not affect the others");

    host.advance(MCDK_STAGE_RUNTIME);
    passed &= expect(sawLine("conformance:run:runtime"), "the runtime-throwing plugin did run before failing");
    {
        // loaded() 按值返回：必须先绑定到具名对象，否则 begin()/end() 来自
        // 两个不同的临时 vector，是悬垂迭代器。
        const auto loaded    = host.loaded();
        const auto degraded  = std::count_if(loaded.begin(), loaded.end(), [](const plugin_host::LoadedPlugin& plugin) {
            return plugin.degraded;
        });
        passed              &= expect(degraded == 2, "throwing `throw 42` at runtime degrades that plugin too");
    }

    // --- on_unload 抛异常：记录后忽略，卸载流程继续 ---------------------
    host.advance(MCDK_STAGE_SHUTDOWN);
    host.shutdown();
    passed &= expect(host.empty(), "every plugin is retired even though one threw during shutdown");
    // 再来一次不能崩。
    host.shutdown();
    passed &= expect(host.empty(), "shutdown stays idempotent after a throwing plugin");

    if (!passed) {
        std::cerr << "--- captured output ---\n";
        for (const auto& line : gOutput) {
            std::cerr << line << '\n';
        }
        std::cerr << "plugin_abi_conformance_test failed\n";
        return 1;
    }
    std::cout << "plugin_abi_conformance_test passed\n";
    return 0;
}
