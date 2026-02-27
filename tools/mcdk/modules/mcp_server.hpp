#pragma once
#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <functional>
#include "./log_buffer.hpp"
#include <nlohmann/json.hpp>
#include <mcp_server.h>

namespace mcdk {

    // MCP服务器配置结构
    struct McpServerConfig {
        bool        enabled    = false;
        std::string serverIp   = "localhost";
        int         serverPort = 19133;
    };

    // 从JSON获取MCP服务器配置
    inline McpServerConfig getMcpServerConfigFromJson(const nlohmann::json& userConfig) {
        McpServerConfig config;
        auto            mcpJson = userConfig.value("mcp_server_config", nlohmann::json::object());
        if (mcpJson.is_object()) {
            config.enabled    = mcpJson.value("enabled", false);
            config.serverIp   = mcpJson.value("server_ip", "localhost");
            config.serverPort = mcpJson.value("server_port", 19133);
        }
        return config;
    }

    // 专为MCBE设计的MCP服务器
    class MCPServer {
    public:
        using CodeExecuteHandler = std::function<bool(const std::string& code, bool isClient)>;
        // 定义单次执行返回状态bool的Handler类型 无参数
        using SimpleHandler = std::function<bool()>;

    private:
        McpServerConfig              config;
        std::shared_ptr<LogBuffer>   logBuffer;          // 用于存储日志的缓冲区
        std::shared_ptr<mcp::server> server;             // MCP服务器实例
        CodeExecuteHandler           codeExecuteHandler; // 代码执行处理器
        SimpleHandler                reloadGameHandler;  // 重载游戏处理器

    public:
        MCPServer(const McpServerConfig& cfg) : config(cfg) {}
        MCPServer(McpServerConfig&& cfg) : config(std::move(cfg)) {}

        void setLogBuffer(std::shared_ptr<LogBuffer> buffer) { logBuffer = std::move(buffer); }
        void setCodeExecuteHandler(CodeExecuteHandler handler) { codeExecuteHandler = std::move(handler); }
        void setReloadGameHandler(SimpleHandler handler) { reloadGameHandler = std::move(handler); }

        static nlohmann::json _logVectorToJson(const std::vector<std::string>& logVector) {
            nlohmann::json jsonArray = nlohmann::json::array();
            for (const auto& log : logVector) {
                jsonArray.push_back({{"type", "text"}, {"text", log}});
            }
            return jsonArray;
        }

        // 初始化日志相关的工具
        void initLogTool() {
            mcp::tool logTool = mcp::tool_builder("get_latest_logs")
                                    .with_description(R"(Returns the most recent game log entries.

Parameters:
- order: "asc" for oldest to newest
         "desc" for newest to oldest
)")
                                    .with_number_param("max_count", "Maximum number of log entries to return", false)
                                    .with_string_param("order", "Order of logs (asc or desc)", false)
                                    .with_read_only_hint(true) // 2025-03-26 annotation
                                    .build();

            server->register_tool(
                logTool,
                [this](const nlohmann::json& params, const std::string& /* session_id */) -> nlohmann::json {
                    size_t      maxCount = params.value("max_count", 100);
                    std::string order    = params.value("order", "asc");
                    if (!logBuffer) {
                        return nlohmann::json{
                            {"isError", true},
                            {"content", nlohmann::json::array({{{"type", "text"}, {"text", "Log buffer not set"}}})}
                        };
                    }
                    if (order == "desc") {
                        return _logVectorToJson(logBuffer->getLatestReversed(maxCount));
                    }
                    return _logVectorToJson(logBuffer->getLatest(maxCount));
                }
            );

            mcp::tool rangeLogTool =
                mcp::tool_builder("get_log_range")
                    .with_description(R"TAG(Returns a specific range of recent game log entries by index.

Logs are indexed relative to the most recent entry:
- index 0 refers to the newest log
- index 1 refers to the second newest log
- and so on

Parameters:
- start_index: Starting index (inclusive)
- end_index: Ending index (exclusive)
- order: "asc" for oldest to newest
         "desc" for newest to oldest)TAG")
                    .with_number_param("start_index", "Starting index (inclusive)", true)
                    .with_number_param("end_index", "Ending index (exclusive)", true)
                    .with_string_param("order", "Order of logs (asc or desc)", false)
                    .with_read_only_hint(true) // 2025-03-26 annotation
                    .build();

            server->register_tool(
                rangeLogTool,
                [this](const nlohmann::json& params, const std::string& /* session_id */) -> nlohmann::json {
                    size_t      startIndex = params.value("start_index", 0);
                    size_t      endIndex   = params.value("end_index", 100);
                    std::string order      = params.value("order", "asc");
                    if (!logBuffer) {
                        return nlohmann::json{
                            {"isError", true},
                            {"content", nlohmann::json::array({{{"type", "text"}, {"text", "Log buffer not set"}}})}
                        };
                    }
                    if (order == "desc") {
                        return _logVectorToJson(logBuffer->getRangeReversed(startIndex, endIndex));
                    }
                    return _logVectorToJson(logBuffer->getRange(startIndex, endIndex));
                }
            );
        }

        // 初始化代码执行相关的工具
        void initCodeExecutionTool() {
            mcp::tool codeExecTool = mcp::tool_builder("execute_code")
                                         .with_description(
                                             R"(Executes provided code in the game environment.
Parameters:
- code: The code to Py2 execute
- is_client: Whether to execute on client side (true) or server side (false))"
                                         )
                                         .with_string_param("code", "Code to execute", true)
                                         .with_boolean_param("is_client", "Execute on client side?", false)
                                         .build();

            server->register_tool(
                codeExecTool,
                [this](const nlohmann::json& params, const std::string& session_id) -> nlohmann::json {
                    if (!codeExecuteHandler) {
                        return nlohmann::json{
                            {"isError", true},
                            {"content",
                             nlohmann::json::array({{{"type", "text"}, {"text", "Code execution handler not set"}}})}
                        };
                    }

                    std::string code     = params.value("code", "");
                    bool        isClient = params.value("is_client", true);

                    bool success = codeExecuteHandler(code, isClient);
                    if (!success) {
                        return nlohmann::json{
                            {"isError", true},
                            {"content",
                             nlohmann::json::array(
                                 {{{"type", "text"},
                                   {"text",
                                    "Code execution failed. The player may not be in the game or the target is "
                                    "unavailable."}}}
                             )}
                        };
                    }

                    return nlohmann::json{
                        {"isError", false},
                        {"content",
                         nlohmann::json::array(
                             {{{"type", "text"},
                               {"text",
                                "Code executed successfully on the target side. Please use get_latest_logs to observe "
                                "the execution result."}}}
                         )}
                    };
                }
            );
        }

        // 初始化游戏相关工具
        void initGameTools() {
            // 提供重新加载游戏的工具
            mcp::tool reloadGameTool =
                mcp::tool_builder("reload_game")
                    .with_description(
                        "Reloads the game environment. Use with caution and only when necessary. Python code supports "
                        "incremental hot-reload, so a full reload should be avoided unless hot-reload is insufficient."
                    )
                    .with_read_only_hint(false) // 2025-03-26 annotation
                    .build();
            server->register_tool(
                reloadGameTool,
                [this](const nlohmann::json& /* params */, const std::string& /* session_id */) -> nlohmann::json {
                    if (reloadGameHandler) {
                        if (reloadGameHandler()) {
                            return nlohmann::json{
                                {"isError", false},
                                {"content",
                                 nlohmann::json::array({{{"type", "text"}, {"text", "Game reload triggered"}}})}
                            };
                        } else {
                            // 也许玩家不在游戏中，无法执行重载
                            return nlohmann::json{
                                {"isError", true},
                                {"content",
                                 nlohmann::json::array(
                                     {{{"type", "text"},
                                       {"text", "Game reload failed. Player may not be in the game."}}}
                                 )}
                            };
                        }
                    } else {
                        return nlohmann::json{
                            {"isError", true},
                            {"content", nlohmann::json::array({{{"type", "text"}, {"text", "Reload handler not set"}}})}
                        };
                    }
                }
            );
        }

        // 启动MCP服务器
        void start() {
            if (!config.enabled || server.get() != nullptr) {
                return;
            }
            mcp::server::configuration srv_conf;
            srv_conf.host = config.serverIp;
            srv_conf.port = config.serverPort;
            server        = std::make_shared<mcp::server>(srv_conf);
            server->set_server_info("Mineraft(BE) MCP Server(MCDK)", "0.1.0");
            // 注册API
            initLogTool();
            initCodeExecutionTool();
            initGameTools();
            server->start(false); // 非阻塞启动
        }

        // 停止MCP服务器
        void stop() {
            if (!config.enabled || server.get() == nullptr) {
                return;
            }
            server->stop();
            server.reset();
        }
    };
} // namespace mcdk