#pragma once
#include <string>
#include <vector>
#include <mcp_server.h>

namespace mcdk {

    inline void test() {
        mcp::server::configuration srv_conf;
        srv_conf.host = "localhost";
        srv_conf.port = 8888;

        mcp::server server(srv_conf);
        server.set_server_info("Minecraft(BE) MCP Server(MCDK)", "0.1.0");

        server.start(false); // Start in non-blocking mode

        server.stop(); // Stop the server when done
    }
} // namespace mcdk