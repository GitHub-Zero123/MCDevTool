#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <mcdevtool/addon.h>

#include "mod_dir_config.hpp"
#include "settings.hpp"

namespace mcdk {

    struct WorldPackReference {
        std::string           packId;
        std::vector<uint32_t> version;
    };

    using WorldPackManifest = std::vector<WorldPackReference>;

    [[nodiscard]] std::filesystem::path normalizeAbsolutePath(const std::filesystem::path& path);
    [[nodiscard]] bool isSamePath(const std::filesystem::path& left, const std::filesystem::path& right);
    [[nodiscard]] bool isWorldProjectDirectory(const std::filesystem::path& path);

    [[nodiscard]] std::optional<std::filesystem::path> resolveWorldSourcePath(
        const WorldSourceConfig&     config,
        const std::filesystem::path& workingDirectory = std::filesystem::current_path()
    );

    [[nodiscard]] std::vector<MCDevTool::Addon::PackInfo>
    collectWorldPacks(const std::filesystem::path& worldSourcePath);

    void
    appendWorldHotReloadModDir(std::vector<UserModDirConfig>& configs, const std::filesystem::path& worldSourcePath);

    [[nodiscard]] std::vector<MCDevTool::Addon::PackInfo> makeHotReloadPacks(
        const std::vector<MCDevTool::Addon::PackInfo>& linkedPacks,
        const std::vector<MCDevTool::Addon::PackInfo>& worldPacks
    );

    [[nodiscard]] bool
    deployWorldSource(const std::filesystem::path& sourcePath, const std::filesystem::path& worldPath, bool resetWorld);

    [[nodiscard]] WorldPackManifest loadWorldPackManifest(
        const std::filesystem::path& worldSourcePath,
        MCDevTool::Addon::PackType   packType,
        std::string_view             preferredFileName
    );

    void mergeLinkedPacksIntoManifest(
        WorldPackManifest&                             behaviorManifest,
        WorldPackManifest&                             resourceManifest,
        const std::vector<MCDevTool::Addon::PackInfo>& linkedPacks
    );

    void writeWorldPackManifest(const std::filesystem::path& path, const WorldPackManifest& manifest);

} // namespace mcdk
