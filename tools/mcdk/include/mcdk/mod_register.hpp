#pragma once

#include <filesystem>
#include <vector>

#include <mcdevtool/addon.h>

#include <mcdk/mod_dir_config.hpp>

namespace mcdk {

    [[nodiscard]] MCDevTool::Addon::PackInfo registerDebugMod();

    void linkUserConfigModDirs(
        std::vector<UserModDirConfig>&           configs,
        std::vector<MCDevTool::Addon::PackInfo>& linkedPacks,
        bool                                     updateConfigPaths = false
    );

} // namespace mcdk
