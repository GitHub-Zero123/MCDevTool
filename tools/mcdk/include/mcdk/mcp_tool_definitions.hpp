#pragma once

#include <vector>

#include <mcp_tool.h>

namespace mcdk::mcp_tool_definitions {

    // 实例标识工具名；bridge 在发现阶段直接调用它来识别每个候选端口背后的游戏。
    inline constexpr auto InstanceInfoName = "mcdk_instance_info";

    [[nodiscard]] mcp::tool              buildGetLatestLogsTool();
    [[nodiscard]] mcp::tool              buildGetLogRangeTool();
    [[nodiscard]] mcp::tool              buildGetLatestErrorLogsTool();
    [[nodiscard]] mcp::tool              buildExecuteCodeTool();
    [[nodiscard]] mcp::tool              buildReloadGameTool();
    [[nodiscard]] mcp::tool              buildCaptureGameWindowTool();
    [[nodiscard]] mcp::tool              buildClickGameWindowTool();
    [[nodiscard]] mcp::tool              buildInstanceInfoTool();
    [[nodiscard]] mcp::tool              buildJsonUiDebuggerTool();
    [[nodiscard]] mcp::tool              buildMcProfilerTool();
    [[nodiscard]] std::vector<mcp::tool> buildAllTools();

} // namespace mcdk::mcp_tool_definitions
