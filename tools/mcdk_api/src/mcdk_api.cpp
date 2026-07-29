#include "mcdk_api.h"

#include "mcdevtool/game_discovery.h"
#include "mcdevtool/utils.h"

#include <mutex>
#include <string>
#include <vector>

namespace {
    std::once_flag                gameExePathsInitFlag;
    std::vector<std::string>      gameExePathStorage;
    std::vector<const char*>      gameExePathView;

    void initializeGameExePaths() {
        const auto& paths = MCDevTool::autoMatchGameExePaths();
        gameExePathStorage.reserve(paths.size());
        for (const auto& path : paths) {
            gameExePathStorage.push_back(MCDevTool::Utils::pathToGenericUtf8(path));
        }

        gameExePathView.reserve(gameExePathStorage.size());
        for (const auto& path : gameExePathStorage) {
            gameExePathView.push_back(path.c_str());
        }
    }
}

extern "C" const char* const* mcdk_api_get_game_exe_paths(size_t* count) noexcept {
    if (count != nullptr) {
        *count = 0;
    }

    try {
        std::call_once(gameExePathsInitFlag, initializeGameExePaths);
        if (count != nullptr) {
            *count = gameExePathView.size();
        }
        return gameExePathView.empty() ? nullptr : gameExePathView.data();
    } catch (...) {
        return nullptr;
    }
}
