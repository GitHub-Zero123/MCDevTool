#include <mcdk/mcp_tool_definitions.hpp>
#include <mcdk/mcp_tool_help.hpp>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
    void require(bool condition, const std::string& message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

    void testEveryToolIsDocumented() {
        const auto tools = mcdk::mcp_tool_definitions::buildAllTools();
        const auto list  = mcdk::mcp_tool_help::formatToolList(tools);
        for (const auto& tool : tools) {
            require(contains(list, tool.name), "list misses " + tool.name);
            const auto help = mcdk::mcp_tool_help::formatToolHelp(tool);
            require(contains(help, tool.description), "help misses the description of " + tool.name);
            require(contains(help, "mcdk call " + tool.name), "help misses the example of " + tool.name);
            for (const auto& [name, property] : tool.parameters_schema["properties"].items()) {
                require(contains(help, name), tool.name + " help misses parameter " + name);
            }
        }
    }

    void testSchemaDetails() {
        const auto help = mcdk::mcp_tool_help::formatToolHelp(mcdk::mcp_tool_definitions::buildExecuteCodeTool());
        require(contains(help, "code (string, 必填)"), "required marker");
        require(contains(help, "is_client (boolean)"), "optional parameter");
        require(contains(help, R"({"code":"..."})"), "example fills required parameters only");

        const auto capture = mcdk::mcp_tool_help::formatToolHelp(mcdk::mcp_tool_definitions::buildCaptureGameWindowTool());
        require(contains(capture, "最小 120"), "numeric bounds");
        require(contains(capture, "    left (number"), "nested properties are indented");
    }

    void testToolWithoutParameters() {
        mcp::tool tool;
        tool.name              = "ping_game";
        tool.description       = "First line\nSecond line";
        tool.parameters_schema = {{"type", "object"}, {"properties", mcp::json::object()}};

        const auto help = mcdk::mcp_tool_help::formatToolHelp(tool);
        require(contains(help, "  无\n"), "no parameters");
        const auto list = mcdk::mcp_tool_help::formatToolList({tool});
        require(contains(list, "First line") && !contains(list, "Second line"), "list keeps the first line only");
    }
} // namespace

int main() {
    try {
        testEveryToolIsDocumented();
        testSchemaDetails();
        testToolWithoutParameters();
    } catch (const std::exception& exception) {
        std::cerr << "FAILED: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "mcp tool help tests passed\n";
    return EXIT_SUCCESS;
}
