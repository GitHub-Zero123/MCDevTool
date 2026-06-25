#pragma once
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <functional>
#include "./log_buffer.hpp"
#include "./mcp_tool_definitions.hpp"
#include "./jsonui_debugger.hpp"
#include "./jsonui_reload_support.hpp"
#include "./game_agent.hpp"
#include <nlohmann/json.hpp>
#include <mcp_server.h>
#include <base64.hpp>
#include <mcdevtool/style.h>

namespace mcdk {

    inline bool writeTextFileUtf8(const std::filesystem::path& path, const std::string& text, std::string& error) {
        try {
            if (!path.is_absolute()) {
                error = "Output path must be absolute.";
                return false;
            }
            auto ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            if (ext != ".svg") {
                error = "Output path must end with .svg.";
                return false;
            }
            const auto parent = path.parent_path();
            if (!parent.empty()) {
                std::filesystem::create_directories(parent);
            }
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file) {
                error = "Failed to open output file.";
                return false;
            }
            file.write(text.data(), static_cast<std::streamsize>(text.size()));
            if (!file) {
                error = "Failed to write output file.";
                return false;
            }
            return true;
        } catch (const std::exception& exc) {
            error = exc.what();
            return false;
        } catch (...) {
            error = "Unknown file write error.";
            return false;
        }
    }

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
        using CodeExecuteHandler = std::function<nlohmann::json(const std::string& code, bool isClient, bool directReturn)>;
        // 定义单次执行返回状态bool的Handler类型 无参数
        using SimpleHandler = std::function<bool()>;
        // 接收一个布尔参数的Handler类型（用于游戏/Addon重载）
        using BoolParamHandler = std::function<bool(bool param)>;

    private:
        McpServerConfig              config;
        std::shared_ptr<LogBuffer>   logBuffer;                 // 用于存储日志的缓冲区
        std::shared_ptr<LogBuffer>   errBuffer;                 // 用于存储错误日志的缓冲区
        std::shared_ptr<mcp::server> server;                    // MCP服务器实例
        CodeExecuteHandler           codeExecuteHandler;        // 代码执行处理器
        BoolParamHandler             reloadGameHandler;         // 重载游戏/Addon处理器
        SimpleHandler                reloadUiHandler;           // 重载 UI definition 处理器
        int                          mcPid = 0;                 // 存储Minecraft进程ID以供后续使用

    public:
        MCPServer(const McpServerConfig& cfg) : config(cfg) {}
        MCPServer(McpServerConfig&& cfg) : config(std::move(cfg)) {}

        void setLogBuffer(std::shared_ptr<LogBuffer> buffer) { logBuffer = std::move(buffer); }
        void setErrBuffer(std::shared_ptr<LogBuffer> buffer) { errBuffer = std::move(buffer); }
        void setCodeExecuteHandler(CodeExecuteHandler handler) { codeExecuteHandler = std::move(handler); }
        void setReloadGameHandler(BoolParamHandler handler) { reloadGameHandler = std::move(handler); }
        void setReloadUiHandler(SimpleHandler handler) { reloadUiHandler = std::move(handler); }
        void setMinecraftProcessId(int pid) { mcPid = pid; }
        int getMinecraftProcessId() const { return mcPid; }

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

                    std::string code         = params.value("code", "");
                    bool        isClient     = params.value("is_client", true);
                    bool        directReturn = params.value("direct_return", true);

                    return codeExecuteHandler(code, isClient, directReturn);
                }
            );
        }

        void initJsonUiDebuggerTool() {
            mcp::tool jsonUiTool = mcp_tool_definitions::buildJsonUiDebuggerTool();

            server->register_tool(
                jsonUiTool,
                [this](const nlohmann::json& params, const std::string& /* session_id */) -> nlohmann::json {
                    const std::string cmd = params.value("cmd", "/help");
                    if (cmd.empty() || cmd.rfind("/help", 0) == 0) {
                        const auto help = jsonui_debugger::buildLocalHelpJson(cmd.empty() ? "/help" : cmd);
                        return nlohmann::json{
                            {"isError", false},
                            {"content", nlohmann::json::array({{{"type", "text"}, {"text", help.dump(2)}}})}
                        };
                    }

                    const std::string trimmedCmd = jsonui_debugger::trimCopy(cmd);
                    if (trimmedCmd == "/reload-ui"
                        || (jsonui_debugger::startsWith(trimmedCmd, "/reload-ui ")
                            && !jsonui_debugger::startsWith(trimmedCmd, "/reload-ui-"))) {
                        nlohmann::json preservePrepare = nullptr;
                        if (jsonui_debugger::commandHasFlag(trimmedCmd, "--preserve-mod-ui")) {
                            if (!codeExecuteHandler) {
                                const auto out = nlohmann::json{
                                    {"ok", false},
                                    {"cmd", "/reload-ui"},
                                    {"error",
                                     {{"code", "CODE_EXECUTION_UNAVAILABLE"},
                                      {"message", "Cannot prepare ModSDK UI preserve transaction without code execution."}}}
                                };
                                return nlohmann::json{
                                    {"isError", true},
                                    {"content", nlohmann::json::array({{{"type", "text"}, {"text", out.dump(2)}}})}
                                };
                            }

                            auto rawPrepare = codeExecuteHandler(
                                jsonui_reload_support::buildPreparePreserveModUiPythonCode(), true, true
                            );
                            if (rawPrepare.value("isError", false)) {
                                return rawPrepare;
                            }

                            std::string prepareText;
                            if (rawPrepare.contains("content") && rawPrepare["content"].is_array()
                                && !rawPrepare["content"].empty()) {
                                const auto& first = rawPrepare["content"][0];
                                if (first.is_object()) {
                                    prepareText = first.value("text", "");
                                }
                            }
                            if (prepareText.empty()) {
                                prepareText = rawPrepare.dump();
                            }
                            preservePrepare = jsonui_debugger::parseFirstJsonFromDirtyText(prepareText);
                            if (!preservePrepare.is_object() || !preservePrepare.value("ok", false)) {
                                const auto out = nlohmann::json{
                                    {"ok", false},
                                    {"cmd", "/reload-ui"},
                                    {"error",
                                     {{"code", "PRESERVE_MOD_UI_PREPARE_FAILED"},
                                      {"message",
                                       "Failed to prepare ModSDK UI preserve transaction; Ctrl+R was not triggered."},
                                      {"prepare", preservePrepare}}}
                                };
                                return nlohmann::json{
                                    {"isError", true},
                                    {"content", nlohmann::json::array({{{"type", "text"}, {"text", out.dump(2)}}})}
                                };
                            }
                        }

                        if (reloadUiHandler && reloadUiHandler()) {
                            const auto out = nlohmann::json{
                                {"ok", true},
                                {"cmd", "/reload-ui"},
                                {"data",
                                 {{"trigger", "Ctrl+R"},
                                  {"preserve_mod_ui", !preservePrepare.is_null()},
                                  {"preserve_prepare", preservePrepare},
                                  {"message",
                                   preservePrepare.is_null()
                                       ? "Native JSON UI definition reload was triggered from the host process."
                                       : "Native JSON UI definition reload was triggered after freezing a temporary "
                                         "ModSDK user UI snapshot; the snapshot will be restored after the engine "
                                         "reload event."},
                                  {"warning",
                                   preservePrepare.is_null()
                                       ? "The engine may reset pushed screens or mod HUD UI; ModSDK ScreenNode state "
                                         "may need a follow-up recovery pass."
                                       : "During reload, ModSDK UI is temporarily cleared and restored from a "
                                         "Python-side snapshot. UiInitFinished is not broadcast."}}}
                            };
                            return nlohmann::json{
                                {"isError", false},
                                {"content", nlohmann::json::array({{{"type", "text"}, {"text", out.dump(2)}}})}
                            };
                        }
                        nlohmann::json rollbackRestore = nullptr;
                        if (!preservePrepare.is_null() && codeExecuteHandler) {
                            auto rawRollback = codeExecuteHandler(
                                jsonui_reload_support::buildRestorePreservedModUiPythonCode(), true, true
                            );
                            std::string rollbackText;
                            if (rawRollback.contains("content") && rawRollback["content"].is_array()
                                && !rawRollback["content"].empty()) {
                                const auto& first = rawRollback["content"][0];
                                if (first.is_object()) {
                                    rollbackText = first.value("text", "");
                                }
                            }
                            if (rollbackText.empty()) {
                                rollbackText = rawRollback.dump();
                            }
                            rollbackRestore = jsonui_debugger::parseFirstJsonFromDirtyText(rollbackText);
                        }
                        const auto out = nlohmann::json{
                            {"ok", false},
                            {"cmd", "/reload-ui"},
                            {"error",
                             {{"code", "RELOAD_UI_FAILED"},
                              {"message",
                               "Failed to trigger Ctrl+R. The game window may not exist, may be minimized, or may "
                               "not accept background key messages."},
                              {"preserve_prepare", preservePrepare},
                              {"rollback_restore", rollbackRestore}}}
                        };
                        return nlohmann::json{
                            {"isError", true},
                            {"content", nlohmann::json::array({{{"type", "text"}, {"text", out.dump(2)}}})}
                        };
                    }

                    if (!codeExecuteHandler) {
                        return nlohmann::json{
                            {"isError", true},
                            {"content",
                             nlohmann::json::array({{{"type", "text"}, {"text", "Code execution handler not set"}}})}
                        };
                    }

                    const std::string code = jsonui_debugger::buildPythonCode(cmd);
                    auto              raw  = codeExecuteHandler(code, true, true);

                    if (raw.value("isError", false)) {
                        return raw;
                    }

                    std::string text;
                    if (raw.contains("content") && raw["content"].is_array() && !raw["content"].empty()) {
                        const auto& first = raw["content"][0];
                        if (first.is_object()) {
                            text = first.value("text", "");
                        }
                    }
                    if (text.empty()) {
                        text = raw.dump();
                    }

                    auto parsed = jsonui_debugger::parseFirstJsonFromDirtyText(text);
                    jsonui_debugger::attachHtmlPseudoIfRequested(cmd, parsed);
                    jsonui_debugger::attachSvgDiagramIfRequested(cmd, parsed);
                    const bool isError = !parsed.is_object() || !parsed.value("ok", false);

                    const bool unsafeSvgImageRequested =
                        jsonui_debugger::commandHasFlag(cmd, "--unsafe-svg-image");
                    const auto outSvgPath = jsonui_debugger::commandOptionValue(cmd, "out");
                    const bool wantsImageFallback =
                        jsonui_debugger::commandHasFlag(cmd, "--image") || unsafeSvgImageRequested
                        || outSvgPath.has_value();

                    if (!isError && wantsImageFallback && parsed.contains("data") && parsed["data"].is_object()
                        && parsed["data"].contains("svg") && parsed["data"]["svg"].is_string()) {
                        const auto  svg = parsed["data"]["svg"].get<std::string>();
                        nlohmann::json textOnly = parsed;
                        bool svgWriteFailed = false;
                        if (textOnly.contains("data") && textOnly["data"].is_object()) {
                            if (outSvgPath.has_value()) {
                                std::string writeError;
                                if (writeTextFileUtf8(std::filesystem::u8path(*outSvgPath), svg, writeError)) {
                                    textOnly["data"]["svg_written"] = true;
                                    textOnly["data"]["svg_path"]    = *outSvgPath;
                                } else {
                                    textOnly["ok"]            = false;
                                    textOnly["data"]["error"] = {
                                        {"code", "SVG_WRITE_FAILED"},
                                        {"message", writeError},
                                        {"path", *outSvgPath}
                                    };
                                    svgWriteFailed = true;
                                }
                            }
                            textOnly["data"].erase("svg");
                            if (unsafeSvgImageRequested) {
                                textOnly["data"]["unsafe_svg_image_disabled"] = true;
                                textOnly["data"]["image_note"] =
                                    "--unsafe-svg-image was requested, but direct MCP image/svg+xml content is disabled "
                                    "by this server to avoid breaking subsequent AI turns in clients that mix tool images "
                                    "into later model context. Use --out=<absolute.svg> for user visual inspection.";
                            } else if (outSvgPath.has_value()) {
                                textOnly["data"]["image_note"] =
                                    "The SVG was written to disk and omitted from the tool text payload to avoid mixing "
                                    "large SVG/image data into later model context.";
                            } else {
                                textOnly["data"]["image_note"] =
                                    "--image requested compact text fallback only. Direct MCP image/svg+xml content is "
                                    "disabled by default because some clients mix tool images into later model context "
                                    "and break subsequent AI turns. Use --out=<absolute.svg> for user visual inspection.";
                            }
                        }

                        return nlohmann::json{
                            {"isError", !textOnly.value("ok", true)},
                            {"content", nlohmann::json::array({{{"type", "text"}, {"text", textOnly.dump(2)}}})}
                        };
                    }

                    return nlohmann::json{
                        {"isError", isError},
                        {"content", nlohmann::json::array({{{"type", "text"}, {"text", parsed.dump(2)}}})}
                    };
                }
            );
        }

        // 初始化游戏相关工具
        void initGameAgentTool() {
            mcp::tool agentTool = mcp_tool_definitions::buildGameAgentTool();

            server->register_tool(
                agentTool,
                [this](const nlohmann::json& params, const std::string& /* session_id */) -> nlohmann::json {
                    const std::string cmd = params.value("cmd", "/help");
                    if (cmd.empty() || cmd.rfind("/help", 0) == 0) {
                        const auto out = nlohmann::json{
                            {"ok", true},
                            {"data", {{"help", game_agent::localHelpText()}}}
                        };
                        return nlohmann::json{
                            {"isError", false},
                            {"content", nlohmann::json::array({{{"type", "text"}, {"text", out.dump(2)}}})}
                        };
                    }

                    if (!codeExecuteHandler) {
                        return nlohmann::json{
                            {"isError", true},
                            {"content",
                             nlohmann::json::array({{{"type", "text"}, {"text", "Code execution handler not set"}}})}
                        };
                    }

                    auto raw = codeExecuteHandler(game_agent::buildPythonCode(cmd), true, true);
                    if (raw.value("isError", false)) {
                        return raw;
                    }

                    std::string text;
                    if (raw.contains("content") && raw["content"].is_array() && !raw["content"].empty()) {
                        const auto& first = raw["content"][0];
                        if (first.is_object()) {
                            text = first.value("text", "");
                        }
                    }
                    if (text.empty()) {
                        text = raw.dump();
                    }

                    auto parsed = jsonui_debugger::parseFirstJsonFromDirtyText(text);
                    const bool isError = !parsed.is_object() || !parsed.value("ok", false);
                    return nlohmann::json{
                        {"isError", isError},
                        {"content", nlohmann::json::array({{{"type", "text"}, {"text", parsed.dump(2)}}})}
                    };
                }
            );
        }

        void initGameTools() {
            // 提供重新加载游戏的工具
            mcp::tool reloadGameTool = mcp_tool_definitions::buildReloadGameTool();
            server->register_tool(
                reloadGameTool,
                [this](const nlohmann::json& params, const std::string& /* session_id */) -> nlohmann::json {
                    const bool reloadAddons = params.value("reload_addons", false);
                    if (reloadGameHandler) {
                        if (reloadGameHandler(reloadAddons)) {
                            const char* message =
                                reloadAddons ? "Addon and game reload triggered" : "Game reload triggered";
                            return nlohmann::json{
                                {"isError", false},
                                {"content",
                                 nlohmann::json::array({{{"type", "text"}, {"text", message}}})}
                            };
                        } else {
                            // 也许玩家不在游戏中，无法执行重载
                            const char* message = reloadAddons
                                                      ? "Addon and game reload failed. Player may not be in the game."
                                                      : "Game reload failed. Player may not be in the game.";
                            return nlohmann::json{
                                {"isError", true},
                                {"content",
                                 nlohmann::json::array({{{"type", "text"}, {"text", message}}})}
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
            initJsonUiDebuggerTool();
            initGameAgentTool();
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
