#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>
#include <vector>

#include <mcdevtool/addon.h>

#include <mcdk/mod_dir_config.hpp>
#include <mcdk/settings.hpp>

namespace mcdk {

    struct GameRunResult {
        bool          started  = false; // false 表示被插件否决，游戏进程没有创建
        std::uint32_t exitCode = 0;
    };

    // 阻塞到游戏退出。
    GameRunResult launchGameExe(
        const std::filesystem::path&                   executablePath,
        std::string_view                               runtimeConfigPath,
        const UserConfig&                              userConfig,
        const std::vector<UserModDirConfig>*           modDirectories,
        const std::vector<MCDevTool::Addon::PackInfo>* linkedPacks
    );

} // namespace mcdk
