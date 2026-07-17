#pragma once

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <mcdevtool/addon.h>
#include <mcdevtool/env.h>
#include <mcdevtool/utils.h>
#include <nlohmann/json.hpp>

#include "mod_dir_config.hpp"

namespace mcdk {
    namespace fs = std::filesystem;

    inline fs::path normalizeAbsolutePath(const fs::path& path) {
        std::error_code ec;
        auto            canonicalPath = fs::weakly_canonical(path, ec);
        if (!ec) {
            return canonicalPath.lexically_normal();
        }
        return fs::absolute(path).lexically_normal();
    }

    inline bool isSamePath(const fs::path& left, const fs::path& right) {
        std::error_code ec;
        if (fs::exists(left, ec) && !ec && fs::exists(right, ec) && !ec) {
            if (fs::equivalent(left, right, ec) && !ec) {
                return true;
            }
        }
        return normalizeAbsolutePath(left) == normalizeAbsolutePath(right);
    }

    inline bool isWorldProjectDirectory(const fs::path& path) {
        std::error_code ec;
        return fs::is_directory(path, ec) && fs::is_regular_file(path / "level.dat", ec);
    }

    // 解析玩法地图源目录。配置为 auto 时，仅检查当前工作目录。
    inline std::optional<fs::path>
    resolveWorldSourcePath(const nlohmann::json& config, const fs::path& workingDirectory = fs::current_path()) {
        const auto setting = config.find("world_source_path");
        if (setting != config.end()
            && (setting->is_null() || (setting->is_string() && setting->get<std::string>().empty()))) {
            return std::nullopt;
        }

        if (setting == config.end() || (setting->is_string() && setting->get<std::string>() == "auto")) {
            if (isWorldProjectDirectory(workingDirectory)) {
                return normalizeAbsolutePath(workingDirectory);
            }
            return std::nullopt;
        }
        if (!setting->is_string()) {
            throw std::runtime_error("world_source_path 必须为路径字符串、auto 或 null。");
        }

        auto sourcePath = fs::u8path(setting->get<std::string>());
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

    inline std::vector<MCDevTool::Addon::PackInfo> collectWorldPacks(const fs::path& worldSourcePath) {
        std::vector<MCDevTool::Addon::PackInfo> packs;
        const std::array                       packContainerNames{"behavior_packs", "resource_packs"};
        for (const auto* containerName : packContainerNames) {
            const auto      containerPath = worldSourcePath / containerName;
            std::error_code ec;
            if (!fs::is_directory(containerPath, ec)) {
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

    inline void
    appendWorldHotReloadModDir(std::vector<UserModDirConfig>& configs, const fs::path& worldSourcePath) {
        const bool alreadyWatched = std::ranges::any_of(configs, [&worldSourcePath](const UserModDirConfig& config) {
            return config.hotReload && isSamePath(config.getAbsolutePath(), worldSourcePath);
        });
        if (!alreadyWatched) {
            configs.emplace_back(worldSourcePath, true, true);
        }
    }

    inline std::vector<MCDevTool::Addon::PackInfo> makeHotReloadPacks(
        const std::vector<MCDevTool::Addon::PackInfo>& linkedPacks,
        const std::vector<MCDevTool::Addon::PackInfo>& worldPacks
    ) {
        auto hotReloadPacks = linkedPacks;
        for (const auto& worldPack : worldPacks) {
            const bool alreadyWatched = std::ranges::any_of(hotReloadPacks, [&worldPack](const auto& pack) {
                return !pack.srcPath.empty() && isSamePath(pack.srcPath, worldPack.srcPath);
            });
            if (!alreadyWatched) {
                hotReloadPacks.push_back(worldPack);
            }
        }
        return hotReloadPacks;
    }

    inline bool shouldSkipWorldSourceEntry(const fs::path& sourceEntry) {
        const auto name = MCDevTool::Utils::pathToUtf8(sourceEntry.filename());
        // 跳过地图根目录的隐藏元数据，不递归过滤行为包或资源包中的内容。
        return !name.empty() && name.front() == '.';
    }

    inline void copyWorldEntry(const fs::path& source, const fs::path& target) {
        std::error_code ec;
        const auto      status = fs::symlink_status(source, ec);
        if (ec) {
            return;
        }
        // 必须先判断链接，避免目录和文件检查跟随链接目标后进入错误分支。
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

    inline void copyWorldPackContainer(const fs::path& source, const fs::path& target) {
        fs::create_directories(target);
        for (const auto& entry : fs::directory_iterator(source)) {
            const auto targetEntry = target / entry.path().filename();
            if (entry.is_directory() && MCDevTool::Addon::parsePackInfo(entry.path())) {
                if (!MCDevTool::createDirectoryJunction(entry.path(), targetEntry)) {
                    std::error_code ec;
                    fs::remove_all(targetEntry, ec);
                    copyWorldEntry(entry.path(), targetEntry);
                    std::cerr << "玩法地图包目录链接创建失败，已回退为复制："
                              << MCDevTool::Utils::pathToUtf8(entry.path().filename()) << "\n";
                }
                continue;
            }
            copyWorldEntry(entry.path(), targetEntry);
        }
    }

    inline void copyWorldSource(const fs::path& source, const fs::path& target) {
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

    // 返回 true 表示本次从源码地图重新部署了世界数据。
    inline bool deployWorldSource(const fs::path& sourcePath, const fs::path& worldPath, bool resetWorld) {
        if (isSamePath(sourcePath, worldPath)) {
            throw std::runtime_error("玩法地图源目录不能与运行时世界目录相同。");
        }
        const bool worldPathExists = fs::exists(worldPath);
        if (!resetWorld && worldPathExists && isWorldProjectDirectory(worldPath)) {
            return false;
        }

        std::error_code ec;
        fs::remove_all(worldPath, ec);
        if (ec) {
            throw std::runtime_error("无法清理旧的运行时世界目录：" + MCDevTool::Utils::pathToGenericUtf8(worldPath));
        }
        copyWorldSource(sourcePath, worldPath);
        if (!isWorldProjectDirectory(worldPath)) {
            throw std::runtime_error("玩法地图部署失败：运行时世界缺少 level.dat。");
        }
        return true;
    }

    inline std::string asciiLower(std::string value) {
        std::ranges::transform(value, value.begin(), [](unsigned char ch) {
            if (ch >= 'A' && ch <= 'Z') {
                return static_cast<char>(ch + ('a' - 'A'));
            }
            return static_cast<char>(ch);
        });
        return value;
    }

    inline bool samePackId(const nlohmann::json& entry, std::string_view packId) {
        return entry.is_object() && asciiLower(entry.value("pack_id", "")) == asciiLower(std::string(packId));
    }

    inline nlohmann::json readPackManifestArray(const fs::path& path) {
        if (!fs::is_regular_file(path)) {
            return nlohmann::json::array();
        }
        std::ifstream input(path, std::ios::binary);
        auto          result = nlohmann::json::parse(input, nullptr, false, true);
        return result.is_array() ? result : nlohmann::json::array();
    }

    inline nlohmann::json loadWorldPackManifest(
        const fs::path&            worldSourcePath,
        MCDevTool::Addon::PackType packType,
        std::string_view           preferredFileName
    ) {
        const std::string standardName = packType == MCDevTool::Addon::PackType::BEHAVIOR ? "world_behavior_packs.json"
                                                                                          : "world_resource_packs.json";
        const std::string neteaseName  = packType == MCDevTool::Addon::PackType::BEHAVIOR
                                           ? "netease_world_behavior_packs.json"
                                           : "netease_world_resource_packs.json";
        const auto preferredPath = worldSourcePath / std::string(preferredFileName);
        if (fs::is_regular_file(preferredPath)) {
            return readPackManifestArray(preferredPath);
        }

        // 优先使用当前启动模式对应的清单；文件不存在时才回退到另一种命名。
        const auto& fallbackName = preferredFileName == standardName ? neteaseName : standardName;
        return readPackManifestArray(worldSourcePath / fallbackName);
    }

    inline void mergeLinkedPacksIntoManifest(
        nlohmann::json&                                behaviorManifest,
        nlohmann::json&                                resourceManifest,
        const std::vector<MCDevTool::Addon::PackInfo>& linkedPacks
    ) {
        for (const auto& pack : linkedPacks) {
            auto* manifest = pack.type == MCDevTool::Addon::PackType::BEHAVIOR ? &behaviorManifest
                           : pack.type == MCDevTool::Addon::PackType::RESOURCE ? &resourceManifest
                                                                               : nullptr;
            if (manifest == nullptr) {
                continue;
            }
            const auto version = nlohmann::json::parse(pack.version, nullptr, false);
            if (!version.is_array()) {
                throw std::runtime_error("Addon 版本格式无效：" + pack.name);
            }
            const auto existing = std::ranges::find_if(*manifest, [&pack](const nlohmann::json& entry) {
                return samePackId(entry, pack.uuid);
            });
            if (existing != manifest->end()) {
                (*existing)["version"] = version;
            } else {
                manifest->push_back({{"pack_id", pack.uuid}, {"version", version}});
            }
        }
    }

} // namespace mcdk
