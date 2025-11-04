#pragma once
#include <filesystem>
#include <vector>
#include "addon.h"

namespace MCDevTool {
    // 获取应用数据目录路径
    std::filesystem::path getAppDataPath();
    // 获取MinecraftPE_Netease数据目录
    std::filesystem::path getMinecraftDataPath();
    // 获取games/com.netease目录
    std::filesystem::path getGamesComNeteasePath();
    // 获取minecraftWorlds目录
    std::filesystem::path getMinecraftWorldsPath();
    // 获取行为包目录
    std::filesystem::path getBehaviorPacksPath();
    // 获取资源包目录
    std::filesystem::path getResourcePacksPath();
    // 获取依赖包目录
    std::filesystem::path getDependenciesPacksPath();
    // 清理运行时行为包目录
    void cleanRuntimeBehaviorPacks();
    // 清理运行时资源包目录
    void cleanRuntimeResourcePacks();
    // 同时清理双pack目录
    void cleanRuntimePacks();
    // 分析并link源代码pack目录到运行时行为/资源包目录(如果成功将返回有效的PackInfo)
    Addon::PackInfo linkSourcePackToRuntimePack(const std::filesystem::path& sourceDir);
    // 分析并link源代码addon目录到运行时行为包目录 该版本支持批量处理目录下的所有pack
    std::vector<Addon::PackInfo> linkSourceAddonToRuntimePacks(const std::filesystem::path& sourceDir);
}   // namespace MCDevTool