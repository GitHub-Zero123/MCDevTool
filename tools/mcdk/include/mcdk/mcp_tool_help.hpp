#pragma once

#include <string>
#include <vector>

#include <mcp_tool.h>

// 帮助文本全部由工具定义生成，新增或修改工具不需要另外维护文档。
namespace mcdk::mcp_tool_help {

    // 每个工具一行：名称和描述首行。
    [[nodiscard]] std::string formatToolList(const std::vector<mcp::tool>& tools);

    // 单个工具的完整说明：描述、参数表和调用示例。
    [[nodiscard]] std::string formatToolHelp(const mcp::tool& tool);

} // namespace mcdk::mcp_tool_help
