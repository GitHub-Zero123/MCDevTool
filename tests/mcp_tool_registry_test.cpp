#include <mcdk/runtime/mcp_tool_registry.hpp>

#include <iostream>
#include <string>
#include <vector>

namespace {

    bool expect(bool condition, const char* description) {
        if (!condition) {
            std::cerr << "Failed: " << description << '\n';
        }
        return condition;
    }

    mcp::tool makeTool(std::string name) {
        mcp::tool tool;
        tool.name              = std::move(name);
        tool.description       = "test tool";
        tool.parameters_schema = mcp::json{{"type", "object"}};
        return tool;
    }

    mcdk::runtime::McpToolHandler makeHandler(std::string marker) {
        return [marker = std::move(marker)](const mcp::json&, const std::string&) -> mcp::json {
            return mcp::json{{"marker", marker}};
        };
    }

} // namespace

int main() {
    using mcdk::runtime::McpToolBindError;
    using mcdk::runtime::McpToolRegistry;

    McpToolRegistry registry;
    bool            passed = true;

    // 参数校验
    passed &= expect(
        registry.bind(makeTool(""), makeHandler("x"), "builtin").error() == McpToolBindError::InvalidName,
        "empty tool name is rejected"
    );
    passed &= expect(
        registry.bind(makeTool("empty_handler"), {}, "builtin").error() == McpToolBindError::EmptyHandler,
        "empty handler is rejected"
    );
    passed &= expect(registry.size() == 0, "rejected registrations leave the registry empty");

    // 正常注册与去重
    passed &= expect(registry.bind(makeTool("alpha"), makeHandler("a"), "builtin").has_value(), "first bind succeeds");
    passed &= expect(
        registry.bind(makeTool("alpha"), makeHandler("a2"), "plugin.x").error() == McpToolBindError::DuplicateName,
        "duplicate tool name is rejected regardless of owner"
    );
    passed &= expect(registry.bind(makeTool("beta"), makeHandler("b"), "plugin.x").has_value(), "second bind succeeds");
    passed &= expect(registry.bind(makeTool("gamma"), makeHandler("c"), "plugin.y").has_value(), "third bind succeeds");
    passed &= expect(registry.size() == 3, "size counts only successful binds");

    // 重名被拒后，先注册者必须原封不动
    const auto* alpha  = registry.find("alpha");
    passed            &= expect(alpha != nullptr, "find locates a registered tool");
    if (alpha != nullptr) {
        passed &= expect(alpha->owner == "builtin", "the first registrant keeps ownership");
        passed &= expect(
            alpha->handler(mcp::json::object(), "session")["marker"] == "a",
            "the first registrant's handler is the one retained"
        );
    }
    passed &= expect(registry.find("does_not_exist") == nullptr, "find returns null for unknown tools");

    // 遍历顺序即注册顺序，用于让发布过程可复现（客户端所见顺序由 mcp::server 按名字排序决定）
    std::vector<std::string> visited;
    registry.forEach([&visited](const mcdk::runtime::McpToolEntry& entry) {
        visited.push_back(entry.descriptor.name);
    });
    passed &= expect(
        visited == std::vector<std::string>{"alpha", "beta", "gamma"},
        "forEach visits tools in registration order"
    );

    // 封存
    passed &= expect(!registry.sealed(), "registry starts unsealed");
    registry.seal();
    passed &= expect(registry.sealed(), "seal() takes effect");
    passed &= expect(
        registry.bind(makeTool("late"), makeHandler("d"), "plugin.z").error() == McpToolBindError::RegistrySealed,
        "binding after seal is rejected"
    );
    passed &= expect(registry.size() == 3, "a rejected late bind does not change the registry");
    passed &= expect(registry.find("beta") != nullptr, "lookups still work after seal");

    if (!passed) {
        std::cerr << "mcp_tool_registry_test failed\n";
        return 1;
    }
    std::cout << "mcp_tool_registry_test passed\n";
    return 0;
}
