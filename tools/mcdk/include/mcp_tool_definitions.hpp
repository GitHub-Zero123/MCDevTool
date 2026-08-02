#pragma once

#include <vector>

#include <mcp_tool.h>

namespace mcdk::mcp_tool_definitions {

    [[nodiscard]] mcp::tool              buildGetLatestLogsTool();
    [[nodiscard]] mcp::tool              buildGetLogRangeTool();
    [[nodiscard]] mcp::tool              buildGetLatestErrorLogsTool();
    [[nodiscard]] mcp::tool              buildExecuteCodeTool();
    [[nodiscard]] mcp::tool              buildReloadGameTool();
    [[nodiscard]] mcp::tool              buildCaptureGameWindowTool();
    [[nodiscard]] mcp::tool              buildClickGameWindowTool();
    [[nodiscard]] mcp::tool              buildJsonUiDebuggerTool();
    [[nodiscard]] std::vector<mcp::tool> buildAllTools();

} // namespace mcdk::mcp_tool_definitions
