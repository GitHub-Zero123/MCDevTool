#include <mcdk/mcp_tool_definitions.hpp>

#include <mcdk/jsonui_debugger.hpp>

namespace mcdk::mcp_tool_definitions {
    using Json = nlohmann::json;

    namespace {
        constexpr auto GetLatestLogsName        = "get_latest_logs";
        constexpr auto GetLatestLogsDescription = R"(Returns the most recent game log entries.

Parameters:
- max_count: Maximum number of log entries to return
- order: "asc" for oldest to newest
         "desc" for newest to oldest
)";

        constexpr auto GetLogRangeName        = "get_log_range";
        constexpr auto GetLogRangeDescription = R"TAG(Returns a specific range of recent game log entries by index.

Logs are indexed relative to the most recent entry:
- index 0 refers to the newest log
- index 1 refers to the second newest log
- and so on

Parameters:
- start_index: Starting index (inclusive)
- end_index: Ending index (exclusive)
- order: "asc" for oldest to newest
         "desc" for newest to oldest)TAG";

        constexpr auto GetLatestErrorLogsName = "get_latest_error_logs";
        constexpr auto GetLatestErrorLogsDescription =
            R"(Returns stderr error log entries only. May not include all errors (e.g., JSON parsing errors). Use get_latest_logs for complete logs.

Parameters:
- max_count: Maximum number of log entries to return
- order: "asc" for oldest to newest, "desc" for newest to oldest
)";

        constexpr auto ExecuteCodeName        = "execute_code";
        constexpr auto ExecuteCodeDescription = R"(Executes provided code in the game environment.
Parameters:
- code: The code to Py2 execute. Expression code returns the expression value; statement code may assign _result to define the returned value.
- is_client: Whether to execute on client side (true) or server side (false)
- direct_return: Whether to wait for and directly return the execution result (default true). Set false to use the legacy async log-based behavior.)";

        constexpr auto ReloadGameName = "reload_game";
        constexpr auto ReloadGameDescription =
            R"(Reloads the running game environment. Prefer automatic Python/UI/Shader/Material hot reload when available; use this only when a full engine-side refresh is still needed.

Parameters:
- reload_addons: When true, trigger the more complete addon-data + game reload path. Use it for resource changes such as JSON, PNG, audio, or other addon files not handled by automatic hot reload.)";

        constexpr auto CaptureGameWindowName = "capture_game_window";
        constexpr auto CaptureGameWindowDescription =
            "Captures the current Minecraft game window as a 480p JPEG screenshot and returns base64-encoded image "
            "data. "
            "This is a relatively expensive visual inspection tool and can distract from code/log based debugging. "
            "For UI structure, layout, node visibility, or JSON UI validation, prefer the specialized jsonui_debugger "
            "tool before using screenshots. Prefer get_latest_logs, get_latest_error_logs, and deterministic file/code "
            "checks first; use screenshots only when the task explicitly requires visual confirmation or logs cannot "
            "answer the question.";

    } // namespace

    mcp::tool buildGetLatestLogsTool() {
        return mcp::tool_builder(GetLatestLogsName)
            .with_description(GetLatestLogsDescription)
            .with_number_param("max_count", "Maximum number of log entries to return", false)
            .with_string_param("order", "Order of logs (asc or desc)", false)
            .with_read_only_hint(true)
            .build();
    }

    mcp::tool buildGetLogRangeTool() {
        return mcp::tool_builder(GetLogRangeName)
            .with_description(GetLogRangeDescription)
            .with_number_param("start_index", "Starting index (inclusive)", true)
            .with_number_param("end_index", "Ending index (exclusive)", true)
            .with_string_param("order", "Order of logs (asc or desc)", false)
            .with_read_only_hint(true)
            .build();
    }

    mcp::tool buildGetLatestErrorLogsTool() {
        return mcp::tool_builder(GetLatestErrorLogsName)
            .with_description(GetLatestErrorLogsDescription)
            .with_number_param("max_count", "Maximum number of log entries to return", false)
            .with_string_param("order", "Order of logs (asc or desc)", false)
            .with_read_only_hint(true)
            .build();
    }

    mcp::tool buildExecuteCodeTool() {
        return mcp::tool_builder(ExecuteCodeName)
            .with_description(ExecuteCodeDescription)
            .with_string_param("code", "Code to execute", true)
            .with_boolean_param("is_client", "Execute on client side?", false)
            .with_boolean_param(
                "direct_return",
                "Directly return execution result instead of relying on logs? Default true.",
                false
            )
            .build();
    }

    mcp::tool buildReloadGameTool() {
        return mcp::tool_builder(ReloadGameName)
            .with_description(ReloadGameDescription)
            .with_boolean_param("reload_addons", "Reload addon data together with the game environment", false)
            .with_read_only_hint(false)
            .build();
    }

    mcp::tool buildCaptureGameWindowTool() {
        return mcp::tool_builder(CaptureGameWindowName)
            .with_description(CaptureGameWindowDescription)
            .with_read_only_hint(true)
            .build();
    }

    mcp::tool buildJsonUiDebuggerTool() {
        return mcp::tool_builder(jsonui_debugger::ToolName)
            .with_description(jsonui_debugger::ToolDescription)
            .with_string_param("cmd", "Command string. Use /help to list commands and usage.", true)
            .with_read_only_hint(true)
            .build();
    }

    mcp::tool buildMcInputTool() {
        mcp::tool tool;
        tool.name = "mc_input";
        tool.description =
            "Drives the Minecraft game window through keyboard and mouse input. One call can run a whole ordered "
            "sequence: clicks, long presses, drags, wheel, camera motion, text and waits. "
            "Use capture='end' and logs='end' to attach a screenshot and recent logs. Call /help first; /state "
            "reports window geometry and whether the game currently holds the pointer. Coordinates default to the "
            "0.0-1.0 percentage space shared with capture_game_window. A successful result means input was dispatched "
            "to the system queue, not that the game reacted - verify with capture_game_window or logs. Input uses "
            "{op:'/...', args:{...}}.";
        tool.parameters_schema = {
            {"type", "object"},
            {"required", Json::array({"op"})},
            {"properties",
             {{"op", {{"type", "string"}, {"description", "Operation such as /help, /state, /run, or /click."}}},
              {"args", {{"type", "object"}, {"description", "Strict operation-specific arguments."}}}}},
            {"additionalProperties", false},
        };
        tool.output_schema = {
            {"type", "object"},
            {"required", Json::array({"ok", "op", "data", "error", "warnings", "next_calls"})},
            {"properties",
             {{"ok", {{"type", "boolean"}}},
              {"op", {{"type", "string"}}},
              {"data", {{"type", Json::array({"object", "null"})}}},
              {"error", {{"type", Json::array({"object", "null"})}}},
              {"warnings", {{"type", "array"}, {"items", {{"type", "object"}}}}},
              {"next_calls", {{"type", "array"}, {"maxItems", 3}, {"items", {{"type", "object"}}}}}}},
            {"additionalProperties", false},
        };
        tool.annotations.read_only_hint   = false;
        tool.annotations.destructive_hint = false;
        tool.annotations.idempotent_hint  = false;
        tool.annotations.open_world_hint  = true;
        return tool;
    }

    mcp::tool buildMcProfilerTool() {
        mcp::tool tool;
        tool.name = "mc_profiler";
        tool.description =
            "Profiles Minecraft Python CPU, Python memory, and optional Native CPU through one bounded command tool. "
            "Native profiles can correlate instrumented Python-facing and engine C++ Tracy zone hierarchies, including "
            "lower-level stages such as data-driven JSON parsing when those zones are emitted. Call /help first. Every "
            "capture has a server deadline; temporary memory results expire after 20 idle minutes, and Markdown/SVG "
            "reports are explicit exports. Results are filtered and paged; same-kind captures support bounded "
            "server-side comparison. Input uses {op:'/...', args:{...}}.";
        tool.parameters_schema = {
            {"type", "object"},
            {"required", Json::array({"op"})},
            {"properties",
             {{"op", {{"type", "string"}, {"description", "Operation such as /help, /doctor, /start, or /query."}}},
              {"args", {{"type", "object"}, {"description", "Strict operation-specific arguments."}}}}},
            {"additionalProperties", false},
        };
        tool.output_schema = {
            {"type", "object"},
            {"required", Json::array({"ok", "op", "job", "data", "error", "warnings", "next_calls"})},
            {"properties",
             {{"ok", {{"type", "boolean"}}},
              {"op", {{"type", "string"}}},
              {"job", {{"type", Json::array({"object", "null"})}}},
              {"data", {{"type", Json::array({"object", "array", "null"})}}},
              {"error", {{"type", Json::array({"object", "null"})}}},
              {"warnings", {{"type", "array"}, {"items", {{"type", "object"}}}}},
              {"next_calls", {{"type", "array"}, {"maxItems", 3}, {"items", {{"type", "object"}}}}}}},
            {"additionalProperties", false},
        };
        tool.annotations.read_only_hint   = false;
        tool.annotations.destructive_hint = true;
        tool.annotations.idempotent_hint  = false;
        tool.annotations.open_world_hint  = true;
        return tool;
    }

    std::vector<mcp::tool> buildAllTools() {
        return {
            buildGetLatestLogsTool(),
            buildGetLogRangeTool(),
            buildGetLatestErrorLogsTool(),
            buildExecuteCodeTool(),
            buildJsonUiDebuggerTool(),
            buildReloadGameTool(),
            buildCaptureGameWindowTool(),
            buildMcInputTool(),
            buildMcProfilerTool(),
        };
    }

} // namespace mcdk::mcp_tool_definitions
