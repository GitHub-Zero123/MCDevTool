#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include <performance/profiler_runtime_owner.hpp>

namespace mcdk::mc_profiler_mcp {

    inline constexpr std::string_view ToolName = "mc_profiler";

    [[nodiscard]] std::optional<nlohmann::json> tryBuildLocalResult(const nlohmann::json& arguments);

    [[nodiscard]] nlohmann::json
    buildErrorResult(std::string_view op, std::string_view code, std::string_view message, bool retryable);

    [[nodiscard]] bool validateProfilerEnvelope(const nlohmann::json& envelope, std::string& error);

    [[nodiscard]] nlohmann::json
    handleRuntimeRequest(performance::ProfilerServiceProvider& provider, const nlohmann::json& arguments);

} // namespace mcdk::mc_profiler_mcp
