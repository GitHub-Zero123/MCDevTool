#pragma once

#include <string_view>

#include <mcdevtool/window_input.h>
#include <nlohmann/json.hpp>

namespace mcdk {
    class LogBuffer;
}

namespace mcdk::mc_input_mcp {

    inline constexpr std::string_view ToolName = "mc_input";

    struct Environment {
        LogBuffer* logBuffer  = nullptr;
        bool       preferPost = false; // 调用方没传 delivery 时用 post
        bool       requirePost = false; // 后台模式：拒绝 sendinput，保证不抢前台、不动鼠标
        MCDevTool::Input::TurnHandler turn;
    };

    // 唯一对外入口。内部兜住全部异常，任何情况下都返回结构化的工具结果，
    // 绝不把异常抛给 MCP 框架变成一句无上下文的协议错误。
    [[nodiscard]] nlohmann::json
    handleRequest(int pid, const nlohmann::json& arguments, const Environment& environment) noexcept;

    [[nodiscard]] inline nlohmann::json
    handleRequest(int pid, const nlohmann::json& arguments, LogBuffer* logBuffer = nullptr) noexcept {
        return handleRequest(pid, arguments, Environment{.logBuffer = logBuffer});
    }

    // 供 stdio 桥接在无法抵达 MCDK 时构造同构的错误信封。
    [[nodiscard]] nlohmann::json
    buildErrorResult(std::string_view op, std::string_view code, std::string_view message, bool retryable);

} // namespace mcdk::mc_input_mcp
