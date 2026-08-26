#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include <mcdevtool/level.h>
#include <mcdk/settings.hpp>

namespace mcdk {

    [[nodiscard]] MCDevTool::Level::ClientVersion readClientVersion(const std::filesystem::path& gameExecutablePath);

    [[nodiscard]] std::vector<uint8_t>
    createUserLevel(const WorldProjectConfig& config, const MCDevTool::Level::ClientVersion& clientVersion);

} // namespace mcdk
