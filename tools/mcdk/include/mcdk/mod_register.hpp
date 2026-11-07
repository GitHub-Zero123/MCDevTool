#pragma once

#include <filesystem>
#include <vector>

#include <mcdevtool/addon.h>
#include <mcdevtool/env.h>

#include <mcdk/mod_dir_config.hpp>

namespace mcdk {

    [[nodiscard]] MCDevTool::Addon::PackInfo registerDebugMod(const MCDevTool::RuntimePackRoots& roots);

    void linkUserConfigModDirs(
        std::vector<UserModDirConfig>&           configs,
        std::vector<MCDevTool::Addon::PackInfo>& linkedPacks,
        const MCDevTool::RuntimePackRoots&       roots,
        bool                                     updateConfigPaths = false
    );

} // namespace mcdk
