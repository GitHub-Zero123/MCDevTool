#pragma once
#include <string_view>
#include <functional>
#include <vector>
#include <thread>
#include <filesystem>
#include <optional>

namespace MCDevTool::HotReload {
    // 监听目标文件夹列表的py文件更新并执行lambda回调
    std::optional<std::thread> watchAndReloadPyFiles(const std::vector<std::filesystem::path>& modDirs,
            const std::function<void(const std::filesystem::path&)>& onFileChanged);

    // 监听目标文件夹列表的py文件更新并执行lambda回调
    std::optional<std::thread> watchAndReloadPyFiles(const std::vector<std::string_view>& modDirs,
            const std::function<void(const std::filesystem::path&)>& onFileChanged);

    // 监听目标pid进程是否回到前台焦点
    std::optional<std::thread> watchProcessForegroundWindow(
        uint32_t pid,
        const std::function<void(bool isForeground)>& onFocusChanged
    );
}