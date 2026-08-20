#include <mcdk/mod_dir_config.hpp>

#include <algorithm>
#include <utility>

#include <nlohmann/json.hpp>

namespace mcdk {
    namespace {
        bool isPathInsideDirectory(const std::filesystem::path& child, const std::filesystem::path& parent) {
            const auto relative = child.lexically_relative(parent);
            if (relative.empty() || relative == ".") {
                return child == parent;
            }
            return *relative.begin() != "..";
        }
    } // namespace

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
            if (config.enabled && config.hotReload) {
                paths.push_back(config.getAbsoluteU8String());
            }
        }
        return paths.dump();
    }

    std::vector<std::filesystem::path> UserModDirConfig::toPathList(const std::vector<UserModDirConfig>& configs) {
        std::vector<std::filesystem::path> paths;
        paths.reserve(configs.size());
        for (const auto& config : configs) {
            if (config.enabled && config.hotReload) {
                paths.push_back(config.getAbsolutePath());
            }
        }
        return paths;
    }

    std::vector<std::filesystem::path> UserModDirConfig::collectHotReloadResourcePackPaths(
        const std::vector<UserModDirConfig>&           configs,
        const std::vector<MCDevTool::Addon::PackInfo>& sourcePacks
    ) {
        const auto                         hotReloadRoots = toPathList(configs);
        std::vector<std::filesystem::path> paths;
        paths.reserve(sourcePacks.size());
        for (const auto& pack : sourcePacks) {
            if (pack.type != MCDevTool::Addon::PackType::RESOURCE || pack.srcPath.empty()) {
                continue;
            }
            const auto packPath = std::filesystem::absolute(pack.srcPath).lexically_normal();
            if (std::ranges::any_of(hotReloadRoots, [&packPath](const auto& root) {
                    return isPathInsideDirectory(packPath, root);
                })) {
                paths.push_back(packPath);
            }
        }
        return paths;
    }

    std::vector<std::filesystem::path> UserModDirConfig::collectResourceSubdirPaths(
        const std::vector<std::filesystem::path>& resourcePackPaths,
        std::string_view                          subdirName
    ) {
        std::vector<std::filesystem::path> paths;
        if (subdirName.empty()) {
            return paths;
        }
        paths.reserve(resourcePackPaths.size());
        for (const auto& resourcePackPath : resourcePackPaths) {
            const auto      targetPath = resourcePackPath / std::filesystem::u8path(std::string(subdirName));
            std::error_code error;
            if (!std::filesystem::is_directory(targetPath, error)) {
                continue;
            }
            paths.push_back(targetPath);
        }
        return paths;
    }

} // namespace mcdk
