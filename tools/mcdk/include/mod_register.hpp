#pragma once

#include <filesystem>
#include <vector>

#include <mcdevtool/addon.h>

#include "mod_dir_config.hpp"
#include "settings.hpp"

namespace mcdk {

    [[nodiscard]] MCDevTool::Addon::PackInfo registerDebugMod(
        const DebugModOptions&               options,
        const std::vector<UserModDirConfig>& modDirectories,
        std::filesystem::path*               outConfigFile = nullptr
    );

    void linkUserConfigModDirs(
        std::vector<UserModDirConfig>&           configs,
        std::vector<MCDevTool::Addon::PackInfo>& linkedPacks,
        bool                                     updateConfigPaths = false
    );

} // namespace mcdk
