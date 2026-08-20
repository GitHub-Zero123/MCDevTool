#include <mcdk/world_project.hpp>

#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <ranges>
#include <stdexcept>
#include <string>

#include <mcdevtool/env.h>
#include <mcdevtool/utils.h>
#include <nlohmann/json.hpp>

namespace mcdk {
    namespace fs = std::filesystem;

    namespace {
        std::string asciiLower(std::string value) {
            std::ranges::transform(value, value.begin(), [](unsigned char character) {
                if (character >= 'A' && character <= 'Z') {
                    return static_cast<char>(character + ('a' - 'A'));
                }
                return static_cast<char>(character);
            });
            return value;
        }

        bool samePackId(const WorldPackReference& entry, std::string_view packId) {
            return asciiLower(entry.packId) == asciiLower(std::string(packId));
        }

        bool shouldSkipWorldSourceEntry(const fs::path& sourceEntry) {
            const auto name = MCDevTool::Utils::pathToUtf8(sourceEntry.filename());
            return !name.empty() && name.front() == '.';
        }

        void copyWorldEntry(const fs::path& source, const fs::path& target) {
            std::error_code error;
            const auto      status = fs::symlink_status(source, error);
            if (error) {
                return;
            }
            if (fs::is_symlink(status)) {
                fs::create_directories(target.parent_path());
                fs::copy(source, target, fs::copy_options::copy_symlinks | fs::copy_options::overwrite_existing);
                return;
            }
            if (fs::is_directory(status)) {
                fs::create_directories(target);
                for (const auto& entry : fs::directory_iterator(source)) {
                    copyWorldEntry(entry.path(), target / entry.path().filename());
                }
                return;
            }
            if (fs::is_regular_file(status)) {
                fs::create_directories(target.parent_path());
                fs::copy_file(source, target, fs::copy_options::overwrite_existing);
            }
        }

        void copyWorldPackContainer(const fs::path& source, const fs::path& target) {
            fs::create_directories(target);
            for (const auto& entry : fs::directory_iterator(source)) {
                const auto targetEntry = target / entry.path().filename();
                if (entry.is_directory() && MCDevTool::Addon::parsePackInfo(entry.path())) {
                    if (!MCDevTool::createDirectoryJunction(entry.path(), targetEntry)) {
                        std::error_code error;
                        fs::remove_all(targetEntry, error);
                        copyWorldEntry(entry.path(), targetEntry);
                        std::cerr << "玩法地图包目录链接创建失败，已回退为复制："
                                  << MCDevTool::Utils::pathToUtf8(entry.path().filename()) << '\n';
                    }
                    continue;
                }
                copyWorldEntry(entry.path(), targetEntry);
            }
        }

        void copyWorldSource(const fs::path& source, const fs::path& target) {
            fs::create_directories(target);
            for (const auto& entry : fs::directory_iterator(source)) {
                if (shouldSkipWorldSourceEntry(entry.path())) {
                    continue;
                }
                const auto targetEntry = target / entry.path().filename();
                const auto name        = MCDevTool::Utils::pathToUtf8(entry.path().filename());
                if (entry.is_directory() && (name == "behavior_packs" || name == "resource_packs")) {
                    copyWorldPackContainer(entry.path(), targetEntry);
                } else {
                    copyWorldEntry(entry.path(), targetEntry);
                }
            }
        }

        WorldPackManifest readPackManifest(const fs::path& path) {
            if (!fs::is_regular_file(path)) {
                return {};
            }
            std::ifstream input(path, std::ios::binary);
            const auto    json = nlohmann::json::parse(input, nullptr, false, true);
            if (!json.is_array()) {
                return {};
            }

            WorldPackManifest manifest;
            manifest.reserve(json.size());
            for (const auto& entry : json) {
                if (!entry.is_object()) {
                    continue;
                }
                const auto version = entry.value("version", nlohmann::json::array());
                if (!version.is_array()) {
                    continue;
                }
                manifest.push_back({
                    .packId  = entry.value("pack_id", ""),
                    .version = version.get<std::vector<uint32_t>>(),
                });
            }
            return manifest;
        }
    } // namespace

    fs::path normalizeAbsolutePath(const fs::path& path) {
        std::error_code error;
        auto            canonicalPath = fs::weakly_canonical(path, error);
        if (!error) {
            return canonicalPath.lexically_normal();
        }
        return fs::absolute(path).lexically_normal();
    }

    bool isSamePath(const fs::path& left, const fs::path& right) {
        std::error_code error;
        if (fs::exists(left, error) && !error && fs::exists(right, error) && !error
            && fs::equivalent(left, right, error) && !error) {
            return true;
        }
        return normalizeAbsolutePath(left) == normalizeAbsolutePath(right);
    }

    bool isWorldProjectDirectory(const fs::path& path) {
        std::error_code error;
        return fs::is_directory(path, error) && fs::is_regular_file(path / "level.dat", error);
    }

    std::optional<fs::path> resolveWorldSourcePath(const WorldSourceConfig& config, const fs::path& workingDirectory) {
        if (config.mode == WorldSourceConfig::Mode::Disabled) {
            return std::nullopt;
        }
        if (config.mode == WorldSourceConfig::Mode::Auto) {
            return isWorldProjectDirectory(workingDirectory)
                     ? std::optional<fs::path>(normalizeAbsolutePath(workingDirectory))
                     : std::nullopt;
        }

        auto sourcePath = config.path;
        if (sourcePath.is_relative()) {
            sourcePath = workingDirectory / sourcePath;
        }
        sourcePath = normalizeAbsolutePath(sourcePath);
        if (!isWorldProjectDirectory(sourcePath)) {
            throw std::runtime_error(
                "玩法地图目录无效（必须包含 level.dat）：" + MCDevTool::Utils::pathToGenericUtf8(sourcePath)
            );
        }
        return sourcePath;
    }

    std::vector<MCDevTool::Addon::PackInfo> collectWorldPacks(const fs::path& worldSourcePath) {
        std::vector<MCDevTool::Addon::PackInfo> packs;
        constexpr std::array                    packContainerNames{"behavior_packs", "resource_packs"};
        for (const auto* containerName : packContainerNames) {
            const auto      containerPath = worldSourcePath / containerName;
            std::error_code error;
            if (!fs::is_directory(containerPath, error)) {
                continue;
            }
            for (const auto& entry : fs::directory_iterator(containerPath)) {
                if (!entry.is_directory()) {
                    continue;
                }
                auto pack = MCDevTool::Addon::parsePackInfo(entry.path());
                if (pack) {
                    const auto packPath = normalizeAbsolutePath(entry.path());
                    pack.path           = packPath;
                    pack.srcPath        = packPath;
                    packs.push_back(std::move(pack));
                }
            }
        }
        return packs;
    }

    void appendWorldHotReloadModDir(std::vector<UserModDirConfig>& configs, const fs::path& worldSourcePath) {
        const bool alreadyConfigured = std::ranges::any_of(configs, [&worldSourcePath](const auto& config) {
            return isSamePath(config.getAbsolutePath(), worldSourcePath);
        });
        if (!alreadyConfigured) {
            configs.emplace_back(worldSourcePath, true, true);
        }
    }

    std::vector<MCDevTool::Addon::PackInfo> makeHotReloadPacks(
        const std::vector<MCDevTool::Addon::PackInfo>& linkedPacks,
        const std::vector<MCDevTool::Addon::PackInfo>& worldPacks
    ) {
        auto result = linkedPacks;
        for (const auto& worldPack : worldPacks) {
            const bool alreadyWatched = std::ranges::any_of(result, [&worldPack](const auto& pack) {
                return !pack.srcPath.empty() && isSamePath(pack.srcPath, worldPack.srcPath);
            });
            if (!alreadyWatched) {
                result.push_back(worldPack);
            }
        }
        return result;
    }

    bool deployWorldSource(const fs::path& sourcePath, const fs::path& worldPath, bool resetWorld) {
        if (isSamePath(sourcePath, worldPath)) {
            throw std::runtime_error("玩法地图源目录不能与运行时世界目录相同。");
        }
        if (!resetWorld && fs::exists(worldPath) && isWorldProjectDirectory(worldPath)) {
            return false;
        }

        std::error_code error;
        fs::remove_all(worldPath, error);
        if (error) {
            throw std::runtime_error("无法清理旧的运行时世界目录：" + MCDevTool::Utils::pathToGenericUtf8(worldPath));
        }
        copyWorldSource(sourcePath, worldPath);
        if (!isWorldProjectDirectory(worldPath)) {
            throw std::runtime_error("玩法地图部署失败：运行时世界缺少 level.dat。");
        }
        return true;
    }

    WorldPackManifest loadWorldPackManifest(
        const fs::path&            worldSourcePath,
        MCDevTool::Addon::PackType packType,
        std::string_view           preferredFileName
    ) {
        const std::string standardName  = packType == MCDevTool::Addon::PackType::BEHAVIOR ? "world_behavior_packs.json"
                                                                                           : "world_resource_packs.json";
        const std::string neteaseName   = packType == MCDevTool::Addon::PackType::BEHAVIOR
                                            ? "netease_world_behavior_packs.json"
                                            : "netease_world_resource_packs.json";
        const auto        preferredPath = worldSourcePath / std::string(preferredFileName);
        if (fs::is_regular_file(preferredPath)) {
            return readPackManifest(preferredPath);
        }
        const auto& fallbackName = preferredFileName == standardName ? neteaseName : standardName;
        return readPackManifest(worldSourcePath / fallbackName);
    }

    void mergeLinkedPacksIntoManifest(
        WorldPackManifest&                             behaviorManifest,
        WorldPackManifest&                             resourceManifest,
        const std::vector<MCDevTool::Addon::PackInfo>& linkedPacks
    ) {
        for (const auto& pack : linkedPacks) {
            auto* manifest = pack.type == MCDevTool::Addon::PackType::BEHAVIOR ? &behaviorManifest
                           : pack.type == MCDevTool::Addon::PackType::RESOURCE ? &resourceManifest
                                                                               : nullptr;
            if (manifest == nullptr) {
                continue;
            }
            const auto existing =
                std::ranges::find_if(*manifest, [&pack](const auto& entry) { return samePackId(entry, pack.uuid); });
            if (existing != manifest->end()) {
                existing->version = pack.version;
            } else {
                manifest->push_back({.packId = pack.uuid, .version = pack.version});
            }
        }
    }

    void writeWorldPackManifest(const fs::path& path, const WorldPackManifest& manifest) {
        auto json = nlohmann::json::array();
        for (const auto& entry : manifest) {
            json.push_back({{"pack_id", entry.packId}, {"version", entry.version}});
        }
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << json.dump(4);
    }

} // namespace mcdk
