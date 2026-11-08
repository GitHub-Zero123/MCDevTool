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

        constexpr auto ReadLogsName = "read_logs";
        constexpr auto ReadLogsDescription =
            R"(Reads game log lines incrementally with a cursor and an optional filter. Prefer this over get_latest_logs: a long session produces far more log than is worth reading, so read only what is new and only what matches.

Every line has an increasing sequence number. Each result ends with "[logs] next=N"; pass N as "since" on the next call to get only lines written after this one. Without "since", the newest "limit" matching lines are returned, which is the usual way to take a first look and obtain a cursor.
"skipped=K" means K lines after "since" were dropped from the in-memory buffer (it keeps about the last 1000 lines, and reload_game clears it) before they could be read. "more" means the limit was reached and the same call with the new cursor continues.

Parameters:
- since: cursor from a previous result (inclusive)
- limit: maximum lines to return, default 50, at most 500
- contains: keep only lines containing this text
- regex: keep only lines matching this ECMAScript regular expression
- ignore_case: apply contains/regex case-insensitively
- stream: "all" (default) or "errors" for stderr only; each stream has its own cursor
- format: "text" (default) or "json" ({lines:[{seq,text}], next, skipped, more}))";

        constexpr auto ExecuteCodeName        = "execute_code";
        constexpr auto ExecuteCodeDescription = R"(Executes provided code in the game environment.
Parameters:
- code: The code to Py2 execute. Expression code returns the expression value; statement code may assign _result to define the returned value.
- is_client: Whether to execute on client side (true) or server side (false)
- direct_return: Whether to wait for and directly return the execution result (default true). Set false to use the legacy async log-based behavior.)";

        constexpr auto ReloadGameName = "reload_game";
        constexpr auto ReloadGameDescription =
            R"(Reloads the running game environment. For Python changes try reload_scripts first. Reload the game when many modules changed together and in-place reload fails because of reload order, or when a full engine-side refresh is needed. The player leaves and re-enters the world; in the CLI, note readyCount from mcdk ps and run mcdk wait --after <readyCount>.

Parameters:
- reload_addons: When true, trigger the more complete addon-data + game reload path. Use it for resource changes such as JSON, PNG, audio, or other addon files not handled by automatic hot reload.)";

        constexpr auto ReloadScriptsName = "reload_scripts";
        constexpr auto ReloadScriptsDescription =
            R"(Synchronously reloads the Python modules whose source files changed since the last reload, and reports which modules were reloaded and which failed (with tracebacks).

Works whether or not the game window has focus. In automated tests, call it after editing scripts instead of waiting for automatic hot reload, so the result is known before the next step. Only modules the game has already imported are reloaded; a brand-new file is loaded when something imports it.

In-place reload replaces modules one at a time, so after changing many files at once (renamed functions, moved classes, changed imports between modules) a module may briefly see another one's old or new version and fail, even though the code is correct. In that case call reload_game to reload the world instead of retrying. Do not add getattr/hasattr checks, try/except fallbacks or other defensive code to make in-place reload pass: that hides real errors and stays in the code after the test.)";

        constexpr auto CaptureGameWindowName = "capture_game_window";
        constexpr auto CaptureGameWindowDescription =
            "Captures the Minecraft game window client area as a JPEG screenshot and returns base64-encoded image "
            "data. Defaults to the whole client area at 480p. "
            "This is a relatively expensive visual inspection tool and can distract from code/log based debugging. "
            "For UI structure, layout, node visibility, or JSON UI validation, prefer the specialized jsonui_debugger "
            "tool before using screenshots. Prefer get_latest_logs, get_latest_error_logs, and deterministic file/code "
            "checks first; use screenshots only when the task explicitly requires visual confirmation or logs cannot "
            "answer the question.\n"
            "Small text such as item counts, tooltips and chat is usually unreadable at the 480p default. To read it, "
            "crop with 'region' instead of enlarging the whole frame: a region is sharper at the same or smaller "
            "payload size. Raise 'max_height' only when the whole frame must stay readable, and remember a larger "
            "image costs proportionally more to send.\n"
            "'region' uses the same 0.0-1.0 client-area coordinates as mc_input, so a point you clicked at (x, y) "
            "sits at the same (x, y) here.";

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

    mcp::tool buildReadLogsTool() {
        return mcp::tool_builder(ReadLogsName)
            .with_description(ReadLogsDescription)
            .with_number_param("since", "Cursor from a previous result's next=", false)
            .with_number_param("limit", "Maximum lines to return (default 50, max 500)", false)
            .with_string_param("contains", "Keep only lines containing this text", false)
            .with_string_param("regex", "Keep only lines matching this ECMAScript regex", false)
            .with_boolean_param("ignore_case", "Case-insensitive contains/regex", false)
            .with_string_param("stream", "all or errors", false)
            .with_string_param("format", "text or json", false)
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

    mcp::tool buildReloadScriptsTool() {
        return mcp::tool_builder(ReloadScriptsName)
            .with_description(ReloadScriptsDescription)
            .with_read_only_hint(false)
            .build();
    }

    mcp::tool buildCaptureGameWindowTool() {
        mcp::tool tool;
        tool.name              = CaptureGameWindowName;
        tool.description       = CaptureGameWindowDescription;
        const Json regionAxis  = {{"type", "number"}, {"minimum", 0.0}, {"maximum", 1.0}};
        tool.parameters_schema = {
            {"type", "object"},
            {"properties",
             {{"max_height",
               {{"type", "integer"},
                {"minimum", 120},
                {"maximum", 1080},
                {"description",
                 "Maximum output height in pixels, default 480. Aspect ratio is kept and small windows are never "
                 "upscaled."}}},
              {"region",
               {{"type", "object"},
                {"description",
                 "Client-area sub-rectangle to capture, in the same 0.0-1.0 coordinates as mc_input. Defaults to the "
                 "whole client area."},
                {"properties",
                 {{"left", regionAxis}, {"top", regionAxis}, {"right", regionAxis}, {"bottom", regionAxis}}},
                {"additionalProperties", false}}}}},
            {"additionalProperties", false},
        };
        tool.annotations.read_only_hint = true;
        return tool;
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
            "0.0-1.0 percentage space shared with capture_game_window. delivery='post' sends input straight to the "
            "game window without taking the foreground or the real mouse, so several instances can run side by side; "
            "look is unavailable in that mode; use the turn step to rotate the camera by degrees. A successful result means input was dispatched "
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
            buildReadLogsTool(),
            buildGetLatestLogsTool(),
            buildGetLogRangeTool(),
            buildGetLatestErrorLogsTool(),
            buildExecuteCodeTool(),
            buildJsonUiDebuggerTool(),
            buildReloadGameTool(),
            buildReloadScriptsTool(),
            buildCaptureGameWindowTool(),
            buildMcInputTool(),
            buildMcProfilerTool(),
        };
    }

} // namespace mcdk::mcp_tool_definitions
