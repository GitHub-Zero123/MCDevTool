#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json_fwd.hpp>

#include "profiler_service.hpp"

namespace mcdk::performance {

    struct GameExecutionError {
        std::string code;
        std::string message;
        bool        retryable = false;
    };

    using GameCodeExecutor = std::function<std::expected<nlohmann::json, GameExecutionError>(
        std::string,
        ProfileTarget,
        std::chrono::milliseconds
    )>;

    struct ProfilerServiceOptions {
        GameCodeExecutor              executeCode;
        std::function<std::uint32_t()> currentGameProcessId;
        std::filesystem::path         storageRoot;
        std::filesystem::path         executableDirectory;
    };

    [[nodiscard]] std::expected<std::shared_ptr<ProfilerService>, ProfilerError>
    createProfilerService(ProfilerServiceOptions options);

} // namespace mcdk::performance
