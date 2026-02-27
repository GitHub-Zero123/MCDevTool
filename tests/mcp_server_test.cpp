#include <iostream>
#include <string>
#include <nlohmann/json.hpp>
#include <mcp_server.h>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using json = nlohmann::json;

static nlohmann::json hello_handler(const json& params, const std::string& /* session_id */) {
    std::string name = "World";
    if (params.contains("name")) {
        name = params["name"].get<std::string>();
    }

    return json::array({{{"type", "text"}, {"text", "Hello, " + name + "!"}}});
}

// Example handler with structured output (2025-03-26)
static nlohmann::json greet_handler(const json& params, const std::string& /* session_id */) {
    std::string name = params.value("name", "World");
    std::string lang = params.value("lang", "en");
    
    std::string greeting;
    if (lang == "zh") greeting = "你好, " + name + "!";
    else if (lang == "ja") greeting = "こんにちは, " + name + "!";
    else greeting = "Hello, " + name + "!";

    // Return structured output (2025-03-26 format)
    return json{
        {"content", json::array({{{"type", "text"}, {"text", greeting}}})},
        {"structuredContent", {{"greeting", greeting}, {"language", lang}}}
    };
}

int main() {
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    mcp::server::configuration srv_conf;
    srv_conf.host = "localhost";
    srv_conf.port = 11451;
    // Streamable HTTP on /mcp (2025-03-26), legacy SSE also enabled
    srv_conf.streamable_endpoint = "/mcp";
    srv_conf.enable_legacy_sse = true;

    mcp::server server(srv_conf);
    server.set_server_info("Mineraft(BE) MCP Server(MCDK)", "0.1.0");

    // Register hello tool (legacy style, works with both 2024-11-05 and 2025-03-26)
    mcp::tool hello_tool = mcp::tool_builder("hello")
                               .with_description("Say hello")
                               .with_string_param("name", "Name to greet", false)
                               .with_read_only_hint(true)     // 2025-03-26 annotation
                               .build();

    server.register_tool(hello_tool, hello_handler);

    // Register greet tool with 2025-03-26 features
    mcp::tool greet_tool = mcp::tool_builder("greet")
                               .with_description("Greet in multiple languages")
                               .with_title("Multilingual Greeter")      // 2025-03-26
                               .with_string_param("name", "Name to greet", true)
                               .with_string_param("lang", "Language code (en/zh/ja)", false)
                               .with_read_only_hint(true)               // 2025-03-26
                               .with_idempotent_hint(true)              // 2025-03-26
                               .with_output_schema({                    // 2025-03-26 structured output
                                   {"type", "object"},
                                   {"properties", {
                                       {"greeting", {{"type", "string"}}},
                                       {"language", {{"type", "string"}}}
                                   }}
                               })
                               .build();

    server.register_tool(greet_tool, greet_handler);

    std::cout << "MCP Server starting on localhost:11451" << std::endl;
    std::cout << "  Streamable HTTP: POST/GET/DELETE /mcp (2025-03-26)" << std::endl;
    std::cout << "  Legacy SSE:      GET /sse + POST /message (2024-11-05)" << std::endl;

    server.start(true);

    server.stop();
    return 0;
}