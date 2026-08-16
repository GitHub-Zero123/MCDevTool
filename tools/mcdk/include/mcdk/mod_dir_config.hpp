#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <mcdevtool/addon.h>

namespace mcdk {

    class UserModDirConfig {
    public:
        std::filesystem::path path;
        bool                  hotReload = false;
        bool                  enabled   = true;

        UserModDirConfig() = default;
        explicit UserModDirConfig(std::filesystem::path path, bool hotReload, bool enabled);

        [[nodiscard]] std::filesystem::path getAbsolutePath() const;
        [[nodiscard]] std::string           getAbsoluteU8String() const;

        [[nodiscard]] static std::vector<UserModDirConfig> fromStringList(const std::vector<std::string>& utf8Paths);

        [[nodiscard]] static std::string toHotReloadListString(const std::vector<UserModDirConfig>& configs);

        [[nodiscard]] static std::vector<std::filesystem::path>
        toPathList(const std::vector<UserModDirConfig>& configs);

        [[nodiscard]] static std::vector<std::filesystem::path> collectHotReloadResourceSubdirPaths(
            const std::vector<MCDevTool::Addon::PackInfo>& sourcePacks,
            std::string_view                               subdirName
        );
    };

} // namespace mcdk
