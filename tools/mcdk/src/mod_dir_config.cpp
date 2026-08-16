#include <mcdk/mod_dir_config.hpp>

#include <utility>

#include <nlohmann/json.hpp>

namespace mcdk {

    UserModDirConfig::UserModDirConfig(std::filesystem::path path, bool hotReload, bool enabled)
    : path(std::move(path)),
      hotReload(hotReload),
      enabled(enabled) {}

    std::filesystem::path UserModDirConfig::getAbsolutePath() const {
        static const auto workingDirectory = std::filesystem::current_path();
        if (path.is_absolute()) {
            return path.lexically_normal();
        }
        return (workingDirectory / path).lexically_normal();
    }

    std::string UserModDirConfig::getAbsoluteU8String() const {
        const auto absolutePath = getAbsolutePath().generic_u8string();
        return {absolutePath.begin(), absolutePath.end()};
    }

    std::vector<UserModDirConfig> UserModDirConfig::fromStringList(const std::vector<std::string>& utf8Paths) {
        std::vector<UserModDirConfig> configs;
        configs.reserve(utf8Paths.size());
        for (const auto& utf8Path : utf8Paths) {
            configs.emplace_back(std::filesystem::u8path(utf8Path), true, true);
        }
        return configs;
    }

    std::string UserModDirConfig::toHotReloadListString(const std::vector<UserModDirConfig>& configs) {
        auto paths = nlohmann::json::array();
        for (const auto& config : configs) {
            if (config.hotReload) {
                paths.push_back(config.getAbsoluteU8String());
            }
        }
        return paths.dump();
    }

    std::vector<std::filesystem::path> UserModDirConfig::toPathList(const std::vector<UserModDirConfig>& configs) {
        std::vector<std::filesystem::path> paths;
        paths.reserve(configs.size());
        for (const auto& config : configs) {
            if (config.hotReload) {
                paths.push_back(config.getAbsolutePath());
            }
        }
        return paths;
    }

    std::vector<std::filesystem::path> UserModDirConfig::collectHotReloadResourceSubdirPaths(
        const std::vector<MCDevTool::Addon::PackInfo>& sourcePacks,
        std::string_view                               subdirName
    ) {
        std::vector<std::filesystem::path> paths;
        for (const auto& pack : sourcePacks) {
            if (pack.type != MCDevTool::Addon::PackType::RESOURCE || pack.srcPath.empty() || subdirName.empty()) {
                continue;
            }
            const auto      targetPath = pack.srcPath / std::filesystem::u8path(std::string(subdirName));
            std::error_code error;
            if (!std::filesystem::is_directory(targetPath, error)) {
                continue;
            }
            paths.push_back(std::filesystem::absolute(targetPath).lexically_normal());
        }
        return paths;
    }

} // namespace mcdk
