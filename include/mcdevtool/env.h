#pragma once
#include "addon.h"
#include "game_discovery.h"
#include <filesystem>
#include <system_error>
#include <vector>

namespace MCDevTool {
    // 运行时包目录：全局目录，或某个存档内的目录。
    struct RuntimePackRoots {
        std::filesystem::path behavior;
        std::filesystem::path resource;
    };

    // 获取应用数据目录路径
    std::filesystem::path getAppDataPath();
    // 让本进程此后的所有数据路径都基于 path，Lane 启动时使用。
    void setAppDataPathOverride(const std::filesystem::path& path);
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
    // 获取全局运行时包目录
    RuntimePackRoots getGlobalPackRoots();
    // 清理运行时行为包目录
    void cleanRuntimeBehaviorPacks();
    // 清理运行时资源包目录
    void cleanRuntimeResourcePacks();
    // 同时清理双pack目录
    void cleanRuntimePacks();
    // 创建目录联接；如果 link 路径已存在，会先删除原路径。
    bool createDirectoryJunction(const std::filesystem::path& target, const std::filesystem::path& link);
    // path 本身是否为联接或符号链接（不跟随）。
    bool isLink(const std::filesystem::path& path);
    // 递归删除 path，遇到联接/符号链接只删链接本身；凡是可能碰到联接的删除都必须走这里，别直接用 remove_all。
    void removeAllNoFollow(const std::filesystem::path& path);
    void removeAllNoFollow(const std::filesystem::path& path, std::error_code& error) noexcept;
    // 分析并link源代码pack目录到运行时行为/资源包目录(如果成功将返回有效的PackInfo)
    Addon::PackInfo linkSourcePackToRuntimePack(const std::filesystem::path& sourceDir);
    Addon::PackInfo linkSourcePackToRuntimePack(const std::filesystem::path& sourceDir, const RuntimePackRoots& roots);
    // 分析并link源代码addon目录到运行时行为包目录 该版本支持批量处理目录下的所有pack
    std::vector<Addon::PackInfo> linkSourceAddonToRuntimePacks(const std::filesystem::path& sourceDir);
    std::vector<Addon::PackInfo>
    linkSourceAddonToRuntimePacks(const std::filesystem::path& sourceDir, const RuntimePackRoots& roots);
} // namespace MCDevTool
