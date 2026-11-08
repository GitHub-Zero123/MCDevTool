#pragma once

#include <nlohmann/json.hpp>

#include <mcdk/log_buffer.hpp>

namespace mcdk::log_query {

    inline constexpr std::size_t DefaultLimit = 50;
    inline constexpr std::size_t MaxLimit     = 500;

    // 按 read_logs 的参数查询，返回 MCP 工具结果（content/isError）。
    [[nodiscard]] nlohmann::json readLogs(const LogBuffer& logs, const LogBuffer& errors, const nlohmann::json& params);

} // namespace mcdk::log_query
