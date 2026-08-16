#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

#include <mcdevtool/addon.h>

#include <mcdk/mod_dir_config.hpp>
#include <mcdk/settings.hpp>

namespace mcdk {

    void launchGameExe(
        const std::filesystem::path&                   executablePath,
        std::string_view                               runtimeConfigPath,
        const UserConfig&                              userConfig,
        const std::vector<UserModDirConfig>*           modDirectories,
        const std::vector<MCDevTool::Addon::PackInfo>* linkedPacks
    );

} // namespace mcdk
