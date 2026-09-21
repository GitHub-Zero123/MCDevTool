// 声明式 MCP 工具：plugin.json 的解析、注册表的 declare/attach、以及
// stdio bridge 用的那条纯磁盘路径。不需要任何插件二进制。
#include <mcdk/plugin_declarations.hpp>
#include <mcdk/runtime/mcp_tool_registry.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <system_error>

#include "../tools/mcdk/src/plugin_host/manifest.hpp"

namespace {

    bool expect(bool condition, const char* description) {
        if (!condition) {
            std::cerr << "Failed: " << description << '\n';
        }
        return condition;
    }

    std::string platformKey() {
#if defined(_WIN32)
        const std::string platform = "windows";
#elif defined(__APPLE__)
        const std::string platform = "macos";
#else
        const std::string platform = "linux";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
        return platform + ".arm64";
#else
        return platform + ".x86_64";
#endif
    }

    void write(const std::filesystem::path& path, const std::string& text) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        output << text;
    }

    std::string manifestWith(const std::string& id, const std::string& toolsJson) {
        return R"({"schema":1,"id":")" + id + R"(","version":"1.0.0","libraries":{")" + platformKey()
             + R"(":"bin/plugin.bin",")" + platformKey() + R"(.debug":"bin/plugin.bin"},"mcpTools":)" + toolsJson
             + "}";
    }

    constexpr auto kOneTool =
        R"([{"name":"demo_echo","description":"回显","inputSchema":{"type":"object"},)"
        R"("annotations":{"readOnlyHint":true,"title":"回显"}}])";

    mcp::json noopHandler(const mcp::json&, const std::string&) { return mcp::json::object(); }

} // namespace

int main() {
    using namespace mcdk;
    namespace ph = mcdk::plugin_host;

    bool       passed = true;
    const auto root   = std::filesystem::temp_directory_path() / "mcdk-mcp-tools-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root);

    // --- 清单解析 ----------------------------------------------------
    {
        write(root / "plugins" / "demo" / "plugin.json", manifestWith("com.demo.tools", kOneTool));
        std::string error;
        const auto  manifest = ph::detail::readManifest(root / "plugins" / "demo", error);
        passed &= expect(manifest.has_value(), "带 mcpTools 的清单能读出来");
        if (manifest) {
            passed &= expect(manifest->mcpTools.size() == 1, "解析出一个工具");
            const auto& tool = manifest->mcpTools.front();
            passed &= expect(tool.name == "demo_echo", "name");
            passed &= expect(tool.description == "回显", "description");
            passed &= expect(tool.parameters_schema.value("type", "") == "object", "inputSchema 原样保留");
            passed &= expect(tool.annotations.read_only_hint.value_or(false), "readOnlyHint 解析成 optional<bool>");
            passed &= expect(
                !tool.annotations.destructive_hint.has_value(),
                "没写的注解仍是「没写」，而不是 false"
            );
            // 清单里写的就是 tools/list 里出现的，中间不经任何改写。
            passed &= expect(
                tool.to_json().value("name", "") == "demo_echo" && tool.to_json().contains("annotations"),
                "to_json 与清单字段同形"
            );
        }
    }
    {
        // 格式错误必须明确失败，不能静默产出一个少了工具的清单。
        write(root / "plugins" / "bad-shape" / "plugin.json", manifestWith("com.demo.bad", R"({"name":"x"})"));
        std::string error;
        passed &= expect(
            !ph::detail::readManifest(root / "plugins" / "bad-shape", error).has_value() && !error.empty(),
            "mcpTools 不是数组时报错"
        );

        write(
            root / "plugins" / "no-schema" / "plugin.json",
            manifestWith("com.demo.noschema", R"([{"name":"x","description":"d"}])")
        );
        error.clear();
        passed &= expect(
            !ph::detail::readManifest(root / "plugins" / "no-schema", error).has_value() && !error.empty(),
            "缺 inputSchema 时报错"
        );

        write(
            root / "plugins" / "dup" / "plugin.json",
            manifestWith(
                "com.demo.dup",
                R"([{"name":"x","inputSchema":{"type":"object"}},{"name":"x","inputSchema":{"type":"object"}}])"
            )
        );
        error.clear();
        passed &= expect(
            !ph::detail::readManifest(root / "plugins" / "dup", error).has_value() && !error.empty(),
            "同一清单内工具重名时报错"
        );
    }

    // --- 注册表 declare / attach -------------------------------------
    {
        runtime::McpToolRegistry registry;
        mcp::tool                tool;
        tool.name              = "demo_echo";
        tool.parameters_schema = mcp::json{{"type", "object"}};

        passed &= expect(registry.declare(tool, "com.demo.tools").has_value(), "declare 成功");
        passed &= expect(registry.unboundTools().size() == 1, "刚声明的工具处于未绑定状态");
        passed &= expect(registry.find("demo_echo") != nullptr, "声明后即可被查到");

        // 占位 handler 必须是可调用的：万一校验被绕过，发布出去的也不能是空函数。
        const auto* entry = registry.find("demo_echo");
        passed &= expect(entry != nullptr && entry->handler != nullptr, "未绑定的条目也有占位 handler");
        if (entry != nullptr) {
            const auto placeholder = entry->handler(mcp::json::object(), "s");
            passed &= expect(placeholder.value("isError", false), "占位 handler 返回错误而不是空结果");
        }

        passed &= expect(
            registry.attach("demo_echo", &noopHandler, "com.other").error()
                == runtime::McpToolBindError::OwnerMismatch,
            "别的插件不能顶替这个工具"
        );
        passed &= expect(
            registry.attach("nope", &noopHandler, "com.demo.tools").error()
                == runtime::McpToolBindError::NotDeclared,
            "绑一个没声明过的名字会失败"
        );
        passed &= expect(registry.attach("demo_echo", &noopHandler, "com.demo.tools").has_value(), "attach 成功");
        passed &= expect(registry.unboundTools().empty(), "绑定后不再是未绑定");
        passed &= expect(
            registry.attach("demo_echo", &noopHandler, "com.demo.tools").error()
                == runtime::McpToolBindError::AlreadyBound,
            "重复 attach 失败，后来者不能覆盖先到者"
        );

        registry.seal();
        passed &= expect(
            registry.declare(tool, "com.demo.tools").error() == runtime::McpToolBindError::RegistrySealed,
            "封存后不能再声明"
        );
    }

    // --- stdio bridge 走的那条纯磁盘路径 ------------------------------
    {
        // 配置文件带注释，和真实的一样。
        write(
            root / ".mcdev.json",
            "{\n  // 注释\n  \"plugins\": [\n"
            "    { \"enable\": true, \"path\": \"plugins/demo\", \"id\": \"com.demo.tools\" },\n"
            "    { \"enable\": false, \"path\": \"plugins/off\" },\n"
            "    { \"enable\": true, \"path\": \"plugins/missing\" }\n  ]\n}\n"
        );
        write(root / "plugins" / "off" / "plugin.json", manifestWith("com.demo.off", R"([{"name":"off_tool","inputSchema":{"type":"object"}}])"));

        const auto tools = ph::detail::declaredMcpTools(root);
        passed &= expect(tools.size() == 1, "只收已启用插件的工具");
        passed &= expect(!tools.empty() && tools.front().name == "demo_echo", "收到的是 demo_echo");

        // 路径不存在的声明只是少几个工具，不该让整个清单查询失败——
        // 调用方是 stdio bridge，它的职责是尽力列出。
        passed &= expect(
            !ph::detail::declaredMcpTools(root / "nowhere").size(),
            "项目目录不存在时返回空而不是抛"
        );
    }

    std::filesystem::remove_all(root, ignored);
    if (!passed) {
        std::cerr << "plugin_mcp_tools_test failed\n";
        return 1;
    }
    return 0;
}
