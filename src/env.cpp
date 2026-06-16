#include "mcdevtool/env.h"
#include "platform/env.h"
#include "mcdevtool/utils.h"
#include <utility> // std::move
#include <iostream>
#include <algorithm>

static void normalizeUUIDString(std::string& uuidStr) {
    uuidStr.erase(std::remove(uuidStr.begin(), uuidStr.end(), '-'), uuidStr.end());
}

namespace MCDevTool {
    // 获取应用数据目录路径
    std::filesystem::path getAppDataPath() { return Platform::getAppDataPath(); }

    // 获取MinecraftPE_Netease数据目录
    std::filesystem::path getMinecraftDataPath() { return getAppDataPath() / "MinecraftPE_Netease"; }

    // 获取games/com.netease目录
    std::filesystem::path getGamesComNeteasePath() { return getMinecraftDataPath() / "games/com.netease"; }

    // 获取minecraftWorlds目录
    std::filesystem::path getMinecraftWorldsPath() { return getMinecraftDataPath() / "minecraftWorlds"; }

    // 获取行为包目录
    std::filesystem::path getBehaviorPacksPath() { return getGamesComNeteasePath() / "behavior_packs"; }

    // 获取资源包目录
    std::filesystem::path getResourcePacksPath() { return getGamesComNeteasePath() / "resource_packs"; }

    // 获取依赖包目录
    std::filesystem::path getDependenciesPacksPath() {
        // 依赖包并非官方目录 仅供开发工具使用
        return getGamesComNeteasePath() / "_dependencies_packs";
    }

    // 自动搜索MCStudioDownload游戏路径（如果存在）
    std::optional<std::filesystem::path> autoSearchMCStudioDownloadGamePath() {
        return Platform::autoSearchMCStudioDownloadGamePath();
    }

    // 自动匹配最新版本游戏可执行文件路径（如果存在）
    std::optional<std::filesystem::path> autoMatchLatestGameExePath() {
        return Platform::autoMatchLatestGameExePath();
    }

    // 清理运行时行为包目录
    void cleanRuntimeBehaviorPacks() {
        auto runtimeBPPath = getBehaviorPacksPath();
        if (std::filesystem::is_directory(runtimeBPPath)) {
            std::filesystem::remove_all(runtimeBPPath);
        }
    }

    // 清理运行时资源包目录
    void cleanRuntimeResourcePacks() {
        auto runtimeRPPath = getResourcePacksPath();
        if (std::filesystem::is_directory(runtimeRPPath)) {
            std::filesystem::remove_all(runtimeRPPath);
        }
    }

    // 同时清理双pack目录
    void cleanRuntimePacks() {
        cleanRuntimeBehaviorPacks();
        cleanRuntimeResourcePacks();
    }

    // 分析并link源代码addon目录到运行时行为/资源包目录
    Addon::PackInfo linkSourcePackToRuntimePack(const std::filesystem::path& sourceDir) {
        auto info = Addon::parsePackInfo(sourceDir);
        if (!info) {
            return info;
        }
        if (info.type == Addon::PackType::BEHAVIOR) {
            auto uuid = info.uuid;
            normalizeUUIDString(uuid);
            auto destPath = getBehaviorPacksPath() / uuid;
            if (!Platform::createPackDirectoryLink(sourceDir, destPath)) {
                std::cerr << "行为包软链接创建失败: " << Utils::pathToUtf8(sourceDir.filename()) << "\n";
            } else {
                info.path = destPath;
            }
        } else if (info.type == Addon::PackType::RESOURCE) {
            auto uuid = info.uuid;
            normalizeUUIDString(uuid);
            auto destPath = getResourcePacksPath() / uuid;
            if (!Platform::createPackDirectoryLink(sourceDir, destPath)) {
                std::cerr << "资源包软链接创建失败: " << Utils::pathToUtf8(sourceDir.filename()) << "\n";
            } else {
                info.path = destPath;
            }
        }
        return info;
    }

    // 分析并link源代码addon目录到运行时行为包目录 该版本支持资源+行为包组合
    std::vector<Addon::PackInfo> linkSourceAddonToRuntimePacks(const std::filesystem::path& sourceDir) {
        std::vector<Addon::PackInfo> packs;
        // 先尝试处理当前目录为单一pack
        auto oncePackInfo = linkSourcePackToRuntimePack(sourceDir);
        if (oncePackInfo) {
            packs.push_back(std::move(oncePackInfo));
            return packs;
        }
        // 遍历当前文件夹下的所有文件夹
        for (const auto& entry : std::filesystem::directory_iterator(sourceDir)) {
            if (entry.is_directory()) {
                auto packInfo = linkSourcePackToRuntimePack(entry.path());
                if (packInfo) {
                    packs.push_back(std::move(packInfo));
                }
            }
        }
        return packs;
    }
} // namespace MCDevTool
