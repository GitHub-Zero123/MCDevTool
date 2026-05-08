#pragma once
#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <functional>
#include "./log_buffer.hpp"
#include "./mcp_tool_definitions.hpp"
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
        std::shared_ptr<LogBuffer>   logBuffer;                 // 用于存储日志的缓冲区
        std::shared_ptr<LogBuffer>   errBuffer;                 // 用于存储错误日志的缓冲区
        std::shared_ptr<mcp::server> server;                    // MCP服务器实例
        CodeExecuteHandler           codeExecuteHandler;        // 代码执行处理器
        SimpleHandler                reloadGameHandler;         // 重载游戏处理器
        SimpleHandler                reloadShadersHandler;      // 重载着色器处理器
        SimpleHandler                reloadAddonAndGameHandler; // 重载插件和游戏处理器
        StringParamHandler           reloadOnceShadersHandler;  // 重载单个着色器处理器
        int                          mcPid = 0;                 // 存储Minecraft进程ID以供后续使用

    public:
        MCPServer(const McpServerConfig& cfg) : config(cfg) {}
        MCPServer(McpServerConfig&& cfg) : config(std::move(cfg)) {}

        void setLogBuffer(std::shared_ptr<LogBuffer> buffer) { logBuffer = std::move(buffer); }
        void setErrBuffer(std::shared_ptr<LogBuffer> buffer) { errBuffer = std::move(buffer); }
        void setCodeExecuteHandler(CodeExecuteHandler handler) { codeExecuteHandler = std::move(handler); }
        void setReloadGameHandler(SimpleHandler handler) { reloadGameHandler = std::move(handler); }
        void setReloadShadersHandler(SimpleHandler handler) { reloadShadersHandler = std::move(handler); }
        void setReloadOnceShadersHandler(StringParamHandler handler) { reloadOnceShadersHandler = std::move(handler); }
        void setReloadAddonAndGameHandler(SimpleHandler handler) { reloadAddonAndGameHandler = std::move(handler); }
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
            mcp::tool logTool = mcp_tool_definitions::buildGetLatestLogsTool();

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

            mcp::tool rangeLogTool = mcp_tool_definitions::buildGetLogRangeTool();

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
            mcp::tool errLogTool = mcp_tool_definitions::buildGetLatestErrorLogsTool();

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
            mcp::tool codeExecTool = mcp_tool_definitions::buildExecuteCodeTool();

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
            mcp::tool reloadGameTool = mcp_tool_definitions::buildReloadGameTool();
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

            // 重新加载游戏以及addon数据（增量贴图/音频之类时需要使用该工具以确保资源被正确重新加载）
            mcp::tool reloadAddonAndGameTool = mcp_tool_definitions::buildReloadAddonAndGameTool();
            server->register_tool(
                reloadAddonAndGameTool,
                [this](const nlohmann::json& /* params */, const std::string& /* session_id */) -> nlohmann::json {
                    if (reloadAddonAndGameHandler) {
                        if (reloadAddonAndGameHandler()) {
                            return nlohmann::json{
                                {"isError", false},
                                {"content",
                                 nlohmann::json::array(
                                     {{{"type", "text"}, {"text", "Addon and game reload triggered"}}}
                                 )}
                            };
                        } else {
                            // 也许玩家不在游戏中，无法执行重载
                            return nlohmann::json{
                                {"isError", true},
                                {"content",
                                 nlohmann::json::array(
                                     {{{"type", "text"},
                                       {"text", "Addon and game reload failed. Player may not be in the game."}}}
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
            mcp::tool reloadShadersTool = mcp_tool_definitions::buildReloadAllShadersTool();
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
            mcp::tool reloadOnceShaderTool = mcp_tool_definitions::buildReloadSingleShaderTool();
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
            mcp::tool captureTool = mcp_tool_definitions::buildCaptureGameWindowTool();

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
            mcp::tool clickTool = mcp_tool_definitions::buildClickGameWindowTool();

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