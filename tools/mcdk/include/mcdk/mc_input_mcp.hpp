#pragma once

#include <string_view>

#include <nlohmann/json.hpp>

namespace mcdk::mc_input_mcp {

    inline constexpr std::string_view ToolName = "mc_input";

    // 唯一对外入口。内部兜住全部异常，任何情况下都返回结构化的工具结果，
    // 绝不把异常抛给 MCP 框架变成一句无上下文的协议错误。
    [[nodiscard]] nlohmann::json handleRequest(int pid, const nlohmann::json& arguments) noexcept;

    // 供 stdio 桥接在无法抵达 MCDK 时构造同构的错误信封。
    [[nodiscard]] nlohmann::json
    buildErrorResult(std::string_view op, std::string_view code, std::string_view message, bool retryable);

} // namespace mcdk::mc_input_mcp
