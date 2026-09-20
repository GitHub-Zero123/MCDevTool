//
// plugin.json 清单解析与依赖拓扑排序（docs/plugin-system/06-loading.md §3、§4）。
//
// 为了得到「多个身份不同的插件」，这里复用 02-loader 那个示例：它按 config 报出
// 动态身份，所以同一个 DLL 被声明多次就能扮演多个插件。这正是 config 字段存在的
// 理由，顺带也让本测试不必再造两个二进制。
//
#include <mcdk/plugin_host/host.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../tools/mcdk/src/plugin_host/manifest.hpp"

#ifndef MCDEV_TEST_SCRIPT_HOST_PLUGIN
#error "MCDEV_TEST_SCRIPT_HOST_PLUGIN must point at the built loader example"
#endif

namespace {

    bool expect(bool condition, const std::string& description) {
        if (!condition) {
            std::cerr << "Failed: " << description << '\n';
        }
        return condition;
    }

    // 在 root 下造一个插件目录：plugin.json + 一份 DLL 副本。
    void makePluginDir(
        const std::filesystem::path& root,
        const std::string&           id,
        const std::string&           version,
        const std::string&           dependenciesJson,
        const std::filesystem::path& sourceLibrary
    ) {
        const auto directory = root / id;
        std::filesystem::create_directories(directory / "bin");
        const auto libraryName = "plugin" + sourceLibrary.extension().string();
        std::filesystem::copy_file(
            sourceLibrary,
            directory / "bin" / libraryName,
            std::filesystem::copy_options::overwrite_existing
        );

        std::ofstream manifest(directory / "plugin.json");
        manifest << "{\n"
                 << R"(  "schema": 1,)" << '\n'
                 << R"(  "id": ")" << id << R"(",)" << '\n'
                 << R"(  "name": ")" << id << R"(",)" << '\n'
                 << R"(  "version": ")" << version << R"(",)" << '\n'
                 << R"(  "libraries": { "windows.x86_64": "bin/)" << libraryName << R"(",)"
                 << R"( "linux.x86_64": "bin/)" << libraryName << R"(" },)" << '\n'
                 << R"(  "dependencies": )" << dependenciesJson << '\n'
                 << "}\n";
    }

    // 02-loader 按 config 里的 id 字段报出身份，这样同一个 DLL 能扮演多个插件。
    std::string configFor(const std::string& id) {
        return R"({"id":")" + id + R"(","name":")" + id + R"(","script":"s.py"})";
    }

    std::vector<std::string> loadedIds(const mcdk::plugin_host::Host& host) {
        std::vector<std::string> result;
        const auto               loaded = host.loaded();
        for (const auto& plugin : loaded) {
            result.push_back(plugin.id);
        }
        return result;
    }

} // namespace

int main() {
    using namespace mcdk;
    namespace ph = mcdk::plugin_host;

    bool passed = true;

    // --- versionSatisfies -------------------------------------------
    {
        using ph::detail::versionSatisfies;
        passed &= expect(versionSatisfies("1.0.0", ""), "空 spec 不限版本");
        passed &= expect(versionSatisfies("1.0.0", "*"), "* 不限版本");
        passed &= expect(versionSatisfies("1.2.3", ">=1.2.0"), ">= 命中");
        passed &= expect(!versionSatisfies("1.1.9", ">=1.2.0"), ">= 未命中");
        passed &= expect(versionSatisfies("2.0", ">=2"), "缺位补 0");
        passed &= expect(!versionSatisfies("2.0.0", ">2.0.0"), "> 是严格大于");
        passed &= expect(versionSatisfies("2.0.1", ">2.0.0"), "> 命中");
        passed &= expect(versionSatisfies("1.0.0", "<=1.0.0"), "<= 含等于");
        passed &= expect(versionSatisfies("1.0.0", "=1.0.0"), "= 精确匹配");
        passed &= expect(versionSatisfies("1.0.0", "1.0.0"), "无运算符即精确匹配");
        // ">=" 必须先于 ">" 被匹配，否则这条会把 "=1.2.0" 当成版本号去解析。
        passed &= expect(versionSatisfies("1.2.0", ">=1.2.0"), ">= 不能被 > 的前缀抢先命中");
        passed &= expect(versionSatisfies("1.0.0-beta", ">=1.0.0"), "预发布标签不参与比较");
    }

    const std::filesystem::path library = MCDEV_TEST_SCRIPT_HOST_PLUGIN;
    const auto root = std::filesystem::temp_directory_path() / "mcdk-manifest-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root);

    // --- readManifest 的错误路径 --------------------------------------
    {
        std::string error;
        const auto  missing = ph::detail::readManifest(root / "nope", error);
        passed             &= expect(!missing.has_value() && !error.empty(), "缺少 plugin.json 时报错而非猜文件名");
    }

    // --- 依赖排序：声明顺序是 app 在前，但 base 必须先加载 ----------------
    makePluginDir(root, "com.test.base", "1.0.0", "[]", library);
    makePluginDir(
        root,
        "com.test.app",
        "1.0.0",
        R"([{"id":"com.test.base","version":">=1.0.0"}])",
        library
    );
    // 依赖不在已启用集合内。
    makePluginDir(root, "com.test.orphan", "1.0.0", R"([{"id":"com.test.absent"}])", library);
    // 成环。
    makePluginDir(root, "com.test.ring-a", "1.0.0", R"([{"id":"com.test.ring-b"}])", library);
    makePluginDir(root, "com.test.ring-b", "1.0.0", R"([{"id":"com.test.ring-a"}])", library);

    std::vector<std::string> output;
    ph::Host host([&output](const std::string& message, ConsoleColor) { output.push_back(message); });

    const auto declare = [&root](const std::string& id) {
        return PluginDeclaration{
            .enabled    = true,
            .path       = (root / id).generic_string(),
            .id         = id,
            .configJson = configFor(id),
            .priority   = 0,
        };
    };

    std::vector<PluginDeclaration> declarations{
        declare("com.test.app"), // 故意排在依赖之前
        declare("com.test.base"),
        declare("com.test.orphan"),
        declare("com.test.ring-a"),
        declare("com.test.ring-b"),
    };
    host.loadDeclared(declarations, root);

    const auto ids = loadedIds(host);
    const auto indexOf = [&ids](const std::string& id) -> std::ptrdiff_t {
        const auto it = std::find(ids.begin(), ids.end(), id);
        return it == ids.end() ? -1 : std::distance(ids.begin(), it);
    };

    passed &= expect(indexOf("com.test.base") >= 0, "base 被加载");
    passed &= expect(indexOf("com.test.app") >= 0, "app 被加载");
    passed &= expect(
        indexOf("com.test.base") < indexOf("com.test.app"),
        "拓扑排序让被依赖者先加载，尽管它在声明里排在后面"
    );
    passed &= expect(indexOf("com.test.orphan") < 0, "依赖不在已启用集合内的插件被跳过");
    passed &= expect(indexOf("com.test.ring-a") < 0 && indexOf("com.test.ring-b") < 0, "成环的插件整环跳过");
    passed &= expect(ids.size() == 2, "只有两个插件真的加载了");

    host.shutdown();
    std::filesystem::remove_all(root, ignored);

    std::cout << (passed ? "plugin_manifest_test passed\n" : "plugin_manifest_test failed\n");
    return passed ? 0 : 1;
}
