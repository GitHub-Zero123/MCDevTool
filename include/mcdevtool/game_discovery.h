#pragma once

#include <filesystem>
#include <optional>
#include <vector>

namespace MCDevTool {
    // Finds the MCStudioDownload game root, if one is available.
    std::optional<std::filesystem::path> autoSearchMCStudioDownloadGamePath();

    // Returns all discovered game executables, ordered by version from newest to oldest.
    const std::vector<std::filesystem::path>& autoMatchGameExePaths();

    // Returns the newest discovered game executable, if one is available.
    std::optional<std::filesystem::path> autoMatchLatestGameExePath();
} // namespace MCDevTool
