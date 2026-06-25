#pragma once

#include <mcp_tool.h>
#include "jsonui_debugger.hpp"
#include "game_agent.hpp"

namespace mcdk::mcp_tool_definitions {

    inline constexpr const char* GetLatestLogsName = "get_latest_logs";
    inline constexpr const char* GetLatestLogsDescription = R"(Returns the most recent game log entries.

Parameters:
- max_count: Maximum number of log entries to return
- order: "asc" for oldest to newest
         "desc" for newest to oldest
)";

    inline constexpr const char* GetLogRangeName = "get_log_range";
    inline constexpr const char* GetLogRangeDescription = R"TAG(Returns a specific range of recent game log entries by index.

Logs are indexed relative to the most recent entry:
- index 0 refers to the newest log
- index 1 refers to the second newest log
- and so on

Parameters:
- start_index: Starting index (inclusive)
- end_index: Ending index (exclusive)
- order: "asc" for oldest to newest
         "desc" for newest to oldest)TAG";

    inline constexpr const char* GetLatestErrorLogsName = "get_latest_error_logs";
    inline constexpr const char* GetLatestErrorLogsDescription = R"(Returns stderr error log entries only. May not include all errors (e.g., JSON parsing errors). Use get_latest_logs for complete logs.

Parameters:
- max_count: Maximum number of log entries to return
- order: "asc" for oldest to newest, "desc" for newest to oldest
)";

    inline constexpr const char* ExecuteCodeName = "execute_code";
    inline constexpr const char* ExecuteCodeDescription = R"(Executes provided code in the game environment.
Parameters:
- code: The code to Py2 execute. Expression code returns the expression value; statement code may assign _result to define the returned value.
- is_client: Whether to execute on client side (true) or server side (false)
- direct_return: Whether to wait for and directly return the execution result (default true). Set false to use the legacy async log-based behavior.)";

    inline constexpr const char* ReloadGameName = "reload_game";
    inline constexpr const char* ReloadGameDescription = R"(Reloads the running game environment. Prefer automatic Python/UI/Shader/Material hot reload when available; use this only when a full engine-side refresh is still needed.

Parameters:
- reload_addons: When true, trigger the more complete addon-data + game reload path. Use it for resource changes such as JSON, PNG, audio, or other addon files not handled by automatic hot reload.)";

    inline constexpr const char* CaptureGameWindowName = "capture_game_window";
    inline constexpr const char* CaptureGameWindowDescription =
        "Captures the current Minecraft game window as a 480p JPEG screenshot and returns base64-encoded image data. "
        "This is a relatively expensive visual inspection tool and can distract from code/log based debugging. "
        "For UI structure, layout, node visibility, or JSON UI validation, prefer the specialized jsonui_debugger tool "
        "before using screenshots. "
        "Prefer get_latest_logs, get_latest_error_logs, and deterministic file/code checks first; use screenshots only "
        "when the task explicitly requires visual confirmation or logs cannot answer the question.";

    inline constexpr const char* ClickGameWindowName = "click_game_window";
    inline constexpr const char* ClickGameWindowDescription = R"(Simulates a left mouse click at a specific position on the Minecraft game window.

Coordinates are percentage-based (0.0 to 1.0) relative to the client area:
- (0.0, 0.0) = top-left corner
- (0.5, 0.5) = center
- (1.0, 1.0) = bottom-right corner

The coordinate system matches the capture_game_window screenshot exactly.
The game window will be brought to the foreground automatically before clicking.

Parameters:
- x: Horizontal position as a percentage (0.0-1.0)
- y: Vertical position as a percentage (0.0-1.0))";

    inline mcp::tool buildGetLatestLogsTool() {
        return mcp::tool_builder(GetLatestLogsName)
            .with_description(GetLatestLogsDescription)
            .with_number_param("max_count", "Maximum number of log entries to return", false)
            .with_string_param("order", "Order of logs (asc or desc)", false)
            .with_read_only_hint(true)
            .build();
    }

    inline mcp::tool buildGetLogRangeTool() {
        return mcp::tool_builder(GetLogRangeName)
            .with_description(GetLogRangeDescription)
            .with_number_param("start_index", "Starting index (inclusive)", true)
            .with_number_param("end_index", "Ending index (exclusive)", true)
            .with_string_param("order", "Order of logs (asc or desc)", false)
            .with_read_only_hint(true)
            .build();
    }

    inline mcp::tool buildGetLatestErrorLogsTool() {
        return mcp::tool_builder(GetLatestErrorLogsName)
            .with_description(GetLatestErrorLogsDescription)
            .with_number_param("max_count", "Maximum number of log entries to return", false)
            .with_string_param("order", "Order of logs (asc or desc)", false)
            .with_read_only_hint(true)
            .build();
    }

    inline mcp::tool buildExecuteCodeTool() {
        return mcp::tool_builder(ExecuteCodeName)
            .with_description(ExecuteCodeDescription)
            .with_string_param("code", "Code to execute", true)
            .with_boolean_param("is_client", "Execute on client side?", false)
            .with_boolean_param("direct_return", "Directly return execution result instead of relying on logs? Default true.", false)
            .build();
    }

    inline mcp::tool buildReloadGameTool() {
        return mcp::tool_builder(ReloadGameName)
            .with_description(ReloadGameDescription)
            .with_boolean_param("reload_addons", "Reload addon data together with the game environment", false)
            .with_read_only_hint(false)
            .build();
    }

    inline mcp::tool buildCaptureGameWindowTool() {
        return mcp::tool_builder(CaptureGameWindowName)
            .with_description(CaptureGameWindowDescription)
            .with_read_only_hint(true)
            .build();
    }

    inline mcp::tool buildClickGameWindowTool() {
        return mcp::tool_builder(ClickGameWindowName)
            .with_description(ClickGameWindowDescription)
            .with_number_param("x", "Horizontal position (0.0=left, 1.0=right)", true)
            .with_number_param("y", "Vertical position (0.0=top, 1.0=bottom)", true)
            .with_read_only_hint(false)
            .build();
    }

    inline mcp::tool buildJsonUiDebuggerTool() {
        return mcp::tool_builder(jsonui_debugger::ToolName)
            .with_description(jsonui_debugger::ToolDescription)
            .with_string_param("cmd", "Command string. Use /help to list commands and usage.", true)
            .with_read_only_hint(true)
            .build();
    }

    inline mcp::tool buildGameAgentTool() {
        return mcp::tool_builder(game_agent::ToolName)
            .with_description(game_agent::ToolDescription)
            .with_string_param("cmd", "Command string. Use /help to list commands and usage.", true)
            .with_read_only_hint(false)
            .build();
    }

    inline std::vector<mcp::tool> buildAllTools() {
        return {
            buildGetLatestLogsTool(),
            buildGetLogRangeTool(),
            buildGetLatestErrorLogsTool(),
            buildExecuteCodeTool(),
            buildJsonUiDebuggerTool(),
            buildGameAgentTool(),
            buildReloadGameTool(),
            buildCaptureGameWindowTool(),
            buildClickGameWindowTool(),
        };
    }

} // namespace mcdk::mcp_tool_definitions
