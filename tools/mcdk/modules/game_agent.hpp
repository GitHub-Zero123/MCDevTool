#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace mcdk::game_agent {

    inline constexpr const char* ToolName = "game_agent";
    inline constexpr const char* ToolDescription = R"(Minecraft runtime agent control through a single command string.

Use cmd="/help" first to learn available commands. The agent runtime is loaded lazily in the in-game Python environment on first use. Regular workflows should call /observe once, then rely on task results and observation_delta instead of repeatedly requesting full state.)";

    inline const char* localHelpText() {
        return R"(game_agent commands:
/help
/observe [--radius=8] [--blocks] [--entities=false] [--inventory=false]
/task start wait [--ticks=20]
/task start run_command <command>
/task start walk_forward [--ticks=20]
/task start move [--forward=1.0] [--strafe=0.0] [--ticks=20]
/task start jump
/task start look --pitch=<pitch> --yaw=<yaw>
/task start look --x=<x> --y=<y> --z=<z>
/task start select_slot --slot=<0-8>
/task start use_item [--release-after=<ticks>]
/task start release_item
/task start use_block <x> <y> <z> [--face=1]
/task start attack_entity <entity_id>
/task get <task_id>
/task list
/task cancel <task_id>
/task cancel-all

The tool exposes one command string to keep MCP context small. Use /observe once to build the first decision baseline; task results include observation_delta and recent events for follow-up decisions.

Mining, placement, and UI clicking task names are reserved by the runtime-agent design, but are not exposed as working tasks until their tick controllers are stable.)";
    }

    inline std::string buildPythonCode(std::string_view cmd) {
        const auto encodedCmd = nlohmann::json(std::string(cmd)).dump();
        return std::string(R"PY(
import json
import importlib
_pkg = globals().get("__package__") or "DEBUG_ENV_SCRIPT"
AgentRuntime = importlib.import_module(_pkg + ".AgentRuntime")
_result = json.dumps(AgentRuntime.handle_cmd()PY")
            + encodedCmd + R"PY(), ensure_ascii=False)
)PY";
    }

} // namespace mcdk::game_agent
