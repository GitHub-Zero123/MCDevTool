#include "mcdevtool/game_discovery.h"
#include "mcdevtool/utils.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

#ifdef _WIN32
#    define NOMINMAX
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>

namespace {
    std::vector<std::string> getAllLogicalDrives() {
        char        buffer[256];
        const DWORD length = GetLogicalDriveStringsA(sizeof(buffer), buffer);
        if (length == 0 || length >= sizeof(buffer)) {
            return {};
        }

        std::vector<std::string> drives;
        for (char* current = buffer; *current != '\0'; current += std::strlen(current) + 1) {
            drives.emplace_back(current);
        }
        return drives;
    }
}
#endif

namespace MCDevTool {
#ifdef _WIN32
    static std::optional<std::filesystem::path> gamePathCache;
    static bool                                 hasNoGamePath = false;

    std::optional<std::filesystem::path> autoSearchMCStudioDownloadGamePath() {
        if (gamePathCache || hasNoGamePath) {
            return gamePathCache;
        }
        for (const auto& drive : getAllLogicalDrives()) {
            const std::filesystem::path path = drive + "MCStudioDownload/game/MinecraftPE_Netease";
            if (!std::filesystem::is_directory(path)) {
                continue;
            }
            for (const auto& entry : std::filesystem::directory_iterator(path)) {
                if (entry.is_directory() && std::filesystem::is_regular_file(entry.path() / "Minecraft.Windows.exe")) {
                    gamePathCache = path;
                    return path;
                }
            }
        }
        hasNoGamePath = true;
        return std::nullopt;
    }
#else
    std::optional<std::filesystem::path> autoSearchMCStudioDownloadGamePath() { return std::nullopt; }
#endif

    static std::optional<std::vector<std::filesystem::path>> cachedGameExePaths;

    const std::vector<std::filesystem::path>& autoMatchGameExePaths() {
        if (cachedGameExePaths) {
            return *cachedGameExePaths;
        }
        const auto gamePath = autoSearchMCStudioDownloadGamePath();
        if (!gamePath) {
            cachedGameExePaths = std::vector<std::filesystem::path>{};
            return *cachedGameExePaths;
        }

        struct GameExeCandidate {
            std::filesystem::path executablePath;
            Utils::Version        version;
        };

        std::vector<GameExeCandidate> candidates;
        std::error_code               error;
        std::filesystem::directory_iterator iterator(
            *gamePath,
            std::filesystem::directory_options::skip_permission_denied,
            error
        );
        const std::filesystem::directory_iterator end;
        while (!error && iterator != end) {
            std::error_code entryError;
            if (iterator->is_directory(entryError)) {
                Utils::Version version{Utils::pathToUtf8(iterator->path().filename())};
                auto           executablePath = iterator->path() / "Minecraft.Windows.exe";
                if (version && std::filesystem::is_regular_file(executablePath, entryError)) {
                    candidates.push_back({std::move(executablePath), std::move(version)});
                }
            }
            iterator.increment(error);
        }

        std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
            if (left.version == right.version) {
                return Utils::pathToGenericUtf8(left.executablePath)
                     < Utils::pathToGenericUtf8(right.executablePath);
            }
            return right.version < left.version;
        });

        std::vector<std::filesystem::path> paths;
        paths.reserve(candidates.size());
        for (auto& candidate : candidates) {
            paths.push_back(std::move(candidate.executablePath));
        }
        cachedGameExePaths = std::move(paths);
        return *cachedGameExePaths;
    }

    std::optional<std::filesystem::path> autoMatchLatestGameExePath() {
        const auto& paths = autoMatchGameExePaths();
        if (paths.empty()) {
            return std::nullopt;
        }
        return paths.front();
    }
} // namespace MCDevTool
