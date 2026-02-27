#include <iostream>
#include <string>
#include <nlohmann/json.hpp>
#include <mcp_server.h>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using json = nlohmann::json;

nlohmann::json hello_handler(const json& params, const std::string& /* session_id */) {
    std::string name = "World";
    if (params.contains("name")) {
        name = params["name"].get<std::string>();
    }

    return json::array({{{"type", "text"}, {"text", "Hello, " + name + "!"}}});
}

int main() {
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    mcp::server::configuration srv_conf;
    srv_conf.host = "localhost";
    srv_conf.port = 11451;

    mcp::server server(srv_conf);
    server.set_server_info("Mineraft(BE) MCP Server(MCDK)", "0.1.0");

    // 注册 hello API
    mcp::tool hello_tool = mcp::tool_builder("hello")
                               .with_description("Say hello")
                               .with_string_param("name", "Name to greet", "World")
                               .build();

    server.register_tool(hello_tool, hello_handler);

    server.start(true);

    server.stop(); // Stop the server when done
    return 0;
}