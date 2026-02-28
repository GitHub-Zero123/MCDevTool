#pragma once
#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <functional>
#include "./log_buffer.hpp"
#include <nlohmann/json.hpp>
#include <mcp_server.h>
#include <base64.hpp>
#include <mcdevtool/style.h>

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
        // 接收一个字符串参数的Handler类型（用于单个着色器重载）
        using StringParamHandler = std::function<bool(const std::string& param)>;

    private:
        McpServerConfig              config;
        std::shared_ptr<LogBuffer>   logBuffer;                // 用于存储日志的缓冲区
        std::shared_ptr<LogBuffer>   errBuffer;                // 用于存储错误日志的缓冲区
        std::shared_ptr<mcp::server> server;                   // MCP服务器实例
        CodeExecuteHandler           codeExecuteHandler;       // 代码执行处理器
        SimpleHandler                reloadGameHandler;        // 重载游戏处理器
        SimpleHandler                reloadShadersHandler;     // 重载着色器处理器
        StringParamHandler           reloadOnceShadersHandler; // 重载单个着色器处理器
        int                          mcPid = 0;                // 存储Minecraft进程ID以供后续使用

    public:
        MCPServer(const McpServerConfig& cfg) : config(cfg) {}
        MCPServer(McpServerConfig&& cfg) : config(std::move(cfg)) {}

        void setLogBuffer(std::shared_ptr<LogBuffer> buffer) { logBuffer = std::move(buffer); }
        void setErrBuffer(std::shared_ptr<LogBuffer> buffer) { errBuffer = std::move(buffer); }
        void setCodeExecuteHandler(CodeExecuteHandler handler) { codeExecuteHandler = std::move(handler); }
        void setReloadGameHandler(SimpleHandler handler) { reloadGameHandler = std::move(handler); }
        void setReloadShadersHandler(SimpleHandler handler) { reloadShadersHandler = std::move(handler); }
        void setReloadOnceShadersHandler(StringParamHandler handler) { reloadOnceShadersHandler = std::move(handler); }
        void setMinecraftProcessId(int pid) { mcPid = pid; }

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
- max_count: Maximum number of log entries to return
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

            // 错误日志查询工具
            // 与普通日志不同，错误日志仅包含stderr的输出，甚至不一定包含非py的错误信息，例如游戏JSON错误等，完整日志需要另外查询普通日志
            mcp::tool errLogTool =
                mcp::tool_builder("get_latest_error_logs")
                    .with_description(
                        R"(Returns stderr error log entries only. May not include all errors (e.g., JSON parsing errors). Use get_latest_logs for complete logs.

Parameters:
- max_count: Maximum number of log entries to return
- order: "asc" for oldest to newest, "desc" for newest to oldest
)"
                    )
                    .with_number_param("max_count", "Maximum number of log entries to return", false)
                    .with_string_param("order", "Order of logs (asc or desc)", false)
                    .with_read_only_hint(true) // 2025-03-26 annotation
                    .build();

            server->register_tool(
                errLogTool,
                [this](const nlohmann::json& params, const std::string& /* session_id */) -> nlohmann::json {
                    size_t      maxCount = params.value("max_count", 100);
                    std::string order    = params.value("order", "asc");
                    if (!errBuffer) {
                        return nlohmann::json{
                            {"isError", true},
                            {"content",
                             nlohmann::json::array({{{"type", "text"}, {"text", "Error log buffer not set"}}})}
                        };
                    }
                    if (order == "desc") {
                        return _logVectorToJson(errBuffer->getLatestReversed(maxCount));
                    }
                    return _logVectorToJson(errBuffer->getLatest(maxCount));
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

            // 重新编译着色器的工具（完整重新编译着色器耗时较长，不建议频繁调用，也不建议盲等完成，可以由用户确认完成后再验证结果）
            mcp::tool reloadShadersTool =
                mcp::tool_builder("reload_all_shaders")
                    .with_description(
                        "Triggers a reload of all shaders. This is a heavy operation and may cause significant lag. "
                        "Use only when necessary, such as after modifying shader files. There is no direct feedback "
                        "when the reload is complete, so please verify the result visually in the game."
                    )
                    .with_read_only_hint(false) // 2025-03-26 annotation
                    .build();
            server->register_tool(
                reloadShadersTool,
                [this](const nlohmann::json& /* params */, const std::string& /* session_id */) -> nlohmann::json {
                    if (reloadShadersHandler) {
                        if (reloadShadersHandler()) {
                            return nlohmann::json{
                                {"isError", false},
                                {"content",
                                 nlohmann::json::array({{{"type", "text"}, {"text", "Shader reload triggered"}}})}
                            };
                        } else {
                            // 也许玩家不在游戏中，无法执行重载
                            return nlohmann::json{
                                {"isError", true},
                                {"content",
                                 nlohmann::json::array(
                                     {{{"type", "text"},
                                       {"text", "Shader reload failed. Player may not be in the game."}}}
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

            // 重新编译单个着色器工具  描述：编译单个着色器文件如："entity.fragment"
            // 以加速编译测试，但如果同时涉及多个文件修改可能会因为依赖关系导致错误，此时应考虑reload_all_shaders
            mcp::tool reloadOnceShaderTool =
                mcp::tool_builder("reload_single_shader")
                    .with_description(
                        R"(Triggers a reload of a single shader by filename. This is faster than reloading all shaders and can be used for quicker iteration when only one shader file is modified.
Parameters:
- file_name: The name of the shader file to reload, relative to the shaders directory.
For example, "entity.fragment" or "block.vertex". Do not include the file extension.)"
                    )
                    .with_string_param("file_name", "Name of the shader file to reload (e.g., 'entity.fragment')", true)
                    .with_read_only_hint(false) // 2025-03-26 annotation
                    .build();
            server->register_tool(
                reloadOnceShaderTool,
                [this](const nlohmann::json& params, const std::string& /* session_id */) -> nlohmann::json {
                    std::string fileName = params.value("file_name", "");
                    if (fileName.empty()) {
                        return nlohmann::json{
                            {"isError", true},
                            {"content",
                             nlohmann::json::array({{{"type", "text"}, {"text", "File name parameter is required"}}})}
                        };
                    }
                    if (reloadOnceShadersHandler) {
                        if (reloadOnceShadersHandler(fileName)) {
                            return nlohmann::json{
                                {"isError", false},
                                {"content",
                                 nlohmann::json::array(
                                     {{{"type", "text"}, {"text", "Single shader reload triggered"}}}
                                 )}
                            };
                        } else {
                            // 也许玩家不在游戏中，无法执行重载
                            return nlohmann::json{
                                {"isError", true},
                                {"content",
                                 nlohmann::json::array(
                                     {{{"type", "text"},
                                       {"text",
                                        "Single shader reload failed. Player may not be in the game or file name may "
                                        "be incorrect."}}}
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

        // 初始化游戏窗口工具（如获取画面，模拟点击）
        void initGameWindowTools() {
            // 截图工具：捕获游戏窗口画面，返回 480p JPEG base64 图片
            mcp::tool captureTool = mcp::tool_builder("capture_game_window")
                                        .with_description(
                                            "Captures the current Minecraft game window as a 480p JPEG screenshot. "
                                            "Returns the image as base64-encoded JPEG data. "
                                            "Use this to observe the current game state visually."
                                        )
                                        .with_read_only_hint(true)
                                        .build();

            server->register_tool(
                captureTool,
                [this](const nlohmann::json& /* params */, const std::string& /* session_id */) -> nlohmann::json {
                    if (mcPid <= 0) {
                        return nlohmann::json{
                            {"isError", true},
                            {"content",
                             nlohmann::json::array(
                                 {{{"type", "text"},
                                   {"text", "Game process ID not set. The game window does not exist."}}}
                             )}
                        };
                    }

                    auto result = MCDevTool::Style::captureMinecraftWindow480p(mcPid);
                    if (!result.has_value() || result->empty()) {
                        return nlohmann::json{
                            {"isError", true},
                            {"content",
                             nlohmann::json::array(
                                 {{{"type", "text"},
                                   {"text",
                                    "Failed to capture game window. "
                                    "The game window does not exist or is minimized."}}}
                             )}
                        };
                    }

                    // 将 JPEG 数据编码为 base64
                    std::string b64 = base64::encode(reinterpret_cast<const char*>(result->data()), result->size());

                    return nlohmann::json{
                        {"isError", false},
                        {"content",
                         nlohmann::json::array({{{"type", "image"}, {"data", b64}, {"mimeType", "image/jpeg"}}})}
                    };
                }
            );

            // 点击工具：模拟点击游戏窗口指定位置
            mcp::tool clickTool =
                mcp::tool_builder("click_game_window")
                    .with_description(
                        R"(Simulates a left mouse click at a specific position on the Minecraft game window.

Coordinates are percentage-based (0.0 to 1.0) relative to the client area:
- (0.0, 0.0) = top-left corner
- (0.5, 0.5) = center
- (1.0, 1.0) = bottom-right corner

The coordinate system matches the capture_game_window screenshot exactly.
The game window will be brought to the foreground automatically before clicking.

Parameters:
- x: Horizontal position as a percentage (0.0-1.0)
- y: Vertical position as a percentage (0.0-1.0))"
                    )
                    .with_number_param("x", "Horizontal position (0.0=left, 1.0=right)", true)
                    .with_number_param("y", "Vertical position (0.0=top, 1.0=bottom)", true)
                    .with_read_only_hint(false)
                    .build();

            server->register_tool(
                clickTool,
                [this](const nlohmann::json& params, const std::string& /* session_id */) -> nlohmann::json {
                    if (mcPid <= 0) {
                        return nlohmann::json{
                            {"isError", true},
                            {"content",
                             nlohmann::json::array(
                                 {{{"type", "text"},
                                   {"text", "Game process ID not set. The game window does not exist."}}}
                             )}
                        };
                    }

                    double x = params.value("x", -1.0);
                    double y = params.value("y", -1.0);

                    if (x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0) {
                        return nlohmann::json{
                            {"isError", true},
                            {"content",
                             nlohmann::json::array(
                                 {{{"type", "text"},
                                   {"text", "Invalid coordinates. x and y must be between 0.0 and 1.0."}}}
                             )}
                        };
                    }

                    bool success = MCDevTool::Style::clickMinecraftWindowAt(mcPid, x, y);
                    if (!success) {
                        return nlohmann::json{
                            {"isError", true},
                            {"content",
                             nlohmann::json::array(
                                 {{{"type", "text"},
                                   {"text",
                                    "Failed to click on game window. "
                                    "The game window does not exist or is minimized."}}}
                             )}
                        };
                    }

                    return nlohmann::json{
                        {"isError", false},
                        {"content",
                         nlohmann::json::array(
                             {{{"type", "text"},
                               {"text",
                                "Click performed at (" + std::to_string(x) + ", " + std::to_string(y)
                                    + "). Use capture_game_window to verify the result."}}}
                         )}
                    };
                }
            );
        }

        // 初始化所有工具
        void initTools() {
            initLogTool();
            initCodeExecutionTool();
            initGameTools();
            initGameWindowTools();
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
            server->set_server_info("Minecraft(BE) MCP Server(MCDK)", "0.1.0");
            // 注册API
            initTools();
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