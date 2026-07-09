#pragma once
#include <string_view>
#include <functional>
#include <vector>
#include <thread>
#include <filesystem>
#include <optional>
#include <atomic>

namespace MCDevTool::HotReload {
    // 文件过滤谓词。注意：它在防抖之前对每条系统通知调用，
    // 而编辑器一次保存往往产生多条通知，因此实现必须廉价且无副作用
    // （仅做路径判断，不要在此读取文件内容或输出日志）。
    // 内容校验、诊断输出等带副作用的逻辑应放在 onFileChanged 中——那里已经过防抖。
    using FileWatchPredicate = std::function<bool(const std::filesystem::path&)>;

    std::optional<std::thread> watchAndReloadFiles(
        const std::vector<std::filesystem::path>&                modDirs,
        const std::function<void(const std::filesystem::path&)>& onFileChanged,
        FileWatchPredicate                                       shouldWatchFile,
        std::atomic<bool>*                                       stopFlag = nullptr
    );

    std::optional<std::thread> watchAndReloadFiles(
        const std::vector<std::string_view>&                     modDirs,
        const std::function<void(const std::filesystem::path&)>& onFileChanged,
        FileWatchPredicate                                       shouldWatchFile,
        std::atomic<bool>*                                       stopFlag = nullptr
    );

    // 监听目标文件夹列表的py文件更新并执行lambda回调
    std::optional<std::thread> watchAndReloadPyFiles(
        const std::vector<std::filesystem::path>&                modDirs,
        const std::function<void(const std::filesystem::path&)>& onFileChanged,
        std::atomic<bool>*                                       stopFlag = nullptr
    );

    // 监听目标文件夹列表的py文件更新并执行lambda回调
    std::optional<std::thread> watchAndReloadPyFiles(
        const std::vector<std::string_view>&                     modDirs,
        const std::function<void(const std::filesystem::path&)>& onFileChanged,
        std::atomic<bool>*                                       stopFlag = nullptr
    );

    // 监听目标pid进程是否回到前台焦点
    std::optional<std::thread> watchProcessForegroundWindow(
        uint32_t                                      pid,
        const std::function<void(bool isForeground)>& onFocusChanged,
        std::atomic<bool>*                            stopFlag = nullptr
    );
} // namespace MCDevTool::HotReload
