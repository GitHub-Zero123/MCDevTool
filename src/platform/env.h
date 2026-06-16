#pragma once

#include <filesystem>
#include <optional>

namespace MCDevTool::Platform {
    std::filesystem::path getAppDataPath();

    bool createPackDirectoryLink(const std::filesystem::path& target, const std::filesystem::path& link);

    std::optional<std::filesystem::path> autoSearchMCStudioDownloadGamePath();

    std::optional<std::filesystem::path> autoMatchLatestGameExePath();
} // namespace MCDevTool::Platform
