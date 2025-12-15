#include "mcdevtool/reload.h"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <iostream>
#include <thread>
#include <atomic>
#include <vector>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <chrono>
#include <mutex>

namespace MCDevTool::HotReload {

#ifdef _WIN32

    namespace fs = std::filesystem;

    struct WatchItem {
        fs::path                    dir;
        HANDLE                      hDir = INVALID_HANDLE_VALUE;
        OVERLAPPED                  ov{};
        std::vector<BYTE>           buffer;
        HANDLE                      eventHandle = nullptr;
        
        // 禁止拷贝，但允许移动
        WatchItem() = default;
        WatchItem(const WatchItem&) = delete;
        WatchItem& operator=(const WatchItem&) = delete;
        WatchItem(WatchItem&& other) noexcept
            : dir(std::move(other.dir))
            , hDir(std::exchange(other.hDir, INVALID_HANDLE_VALUE))
            , ov(other.ov)
            , buffer(std::move(other.buffer))
            , eventHandle(std::exchange(other.eventHandle, nullptr))
        {
            // 重置 ov 的 hEvent 指向自己
            ov.hEvent = eventHandle;
        }
        WatchItem& operator=(WatchItem&& other) noexcept {
            if (this != &other) {
                dir = std::move(other.dir);
                hDir = std::exchange(other.hDir, INVALID_HANDLE_VALUE);
                ov = other.ov;
                buffer = std::move(other.buffer);
                eventHandle = std::exchange(other.eventHandle, nullptr);
                ov.hEvent = eventHandle;
            }
            return *this;
        }
    };

    // 仅监听文件内容修改，不监听新增/删除/重命名
    static constexpr DWORD WATCH_FILTER = FILE_NOTIFY_CHANGE_LAST_WRITE;
    
    // 防抖时间间隔（毫秒）
    static constexpr int DEBOUNCE_MS = 100;

    static constexpr DWORD BUFFER_SIZE = 64 * 1024;

    // ------------------------------------------------------------

    static bool startWatch(WatchItem& item) {
        DWORD bytesReturned = 0;
        return ReadDirectoryChangesW(
            item.hDir,
            item.buffer.data(),
            static_cast<DWORD>(item.buffer.size()),
            TRUE, // recursive
            WATCH_FILTER,
            &bytesReturned,
            &item.ov,
            nullptr
        );
    }

    // ------------------------------------------------------------

    std::optional<std::thread> watchAndReloadPyFiles(
        const std::vector<fs::path>& modDirs,
        const std::function<void(const fs::path&)>& onFileChanged
    ) {
        if (modDirs.empty()) {
            return std::nullopt;
        }

        std::vector<WatchItem> items;
        items.reserve(modDirs.size());

        for (const auto& dir : modDirs) {
            if (!fs::exists(dir) || !fs::is_directory(dir)) {
                continue;
            }

            WatchItem item;
            item.dir = fs::absolute(dir);
            item.buffer.resize(BUFFER_SIZE);

            item.eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!item.eventHandle)
                continue;

            item.hDir = CreateFileW(
                item.dir.wstring().c_str(),
                FILE_LIST_DIRECTORY,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                nullptr
            );

            if (item.hDir == INVALID_HANDLE_VALUE) {
                CloseHandle(item.eventHandle);
                continue;
            }

            ZeroMemory(&item.ov, sizeof(item.ov));
            item.ov.hEvent = item.eventHandle;

            // 注意：不在这里调用 startWatch，移动到线程内部首次调用
            // 因为异步操作会使用 OVERLAPPED 结构的地址，移动后地址会变

            items.emplace_back(std::move(item));
        }

        if (items.empty()) {
            return std::nullopt;
        }

        // 后台监听线程
        return std::thread([items = std::move(items), onFileChanged]() mutable {

            std::vector<HANDLE> waitHandles;
            waitHandles.reserve(items.size());

            for (auto& i : items) {
                waitHandles.push_back(i.eventHandle);
            }

            // 在线程内首次启动监听（此时 items 已经不会再移动）
            for (auto& i : items) {
                if (!startWatch(i)) {
                    std::cerr << "[ERROR] Failed to start watch for: " << i.dir << std::endl;
                }
            }

            // 防抖：记录每个文件的最后触发时间
            std::unordered_map<std::wstring, std::chrono::steady_clock::time_point> lastTriggerTime;
            std::mutex debounceMapMutex;

            while (true) {
                DWORD index = WaitForMultipleObjects(
                    static_cast<DWORD>(waitHandles.size()),
                    waitHandles.data(),
                    FALSE,
                    INFINITE
                );

                // std::cerr << "[DEBUG] WaitForMultipleObjects returned: " << index << std::endl;

                if (index == WAIT_FAILED) {
                    std::cerr << "[DEBUG] WAIT_FAILED, GetLastError=" << GetLastError() << std::endl;
                    break;
                }

                index -= WAIT_OBJECT_0;
                if (index >= items.size()) {
                    continue;
                }

                auto& item = items[index];

                DWORD bytes = 0;
                if (!GetOverlappedResult(item.hDir, &item.ov, &bytes, FALSE)) {
                    continue;
                }

                BYTE* ptr = item.buffer.data();
                auto now = std::chrono::steady_clock::now();

                while (true) {
                    auto* fni = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(ptr);

                    std::wstring name(
                        fni->FileName,
                        fni->FileNameLength / sizeof(wchar_t)
                    );

                    fs::path fullPath = item.dir / name;

                    // std::wcerr << L"[DEBUG] Action=" << fni->Action << L" File=" << fullPath.wstring() << std::endl;

                    // 仅处理文件修改事件 (FILE_ACTION_MODIFIED)
                    // 当监听 FILE_NOTIFY_CHANGE_LAST_WRITE 时，Action 仍然是 FILE_ACTION_MODIFIED
                    if (fni->Action == FILE_ACTION_MODIFIED && fullPath.extension() == L".py") {
                        std::wstring pathKey = fullPath.wstring();
                        bool shouldTrigger = false;

                        {
                            std::lock_guard<std::mutex> lock(debounceMapMutex);
                            auto it = lastTriggerTime.find(pathKey);
                            if (it == lastTriggerTime.end()) {
                                shouldTrigger = true;
                                lastTriggerTime[pathKey] = now;
                            } else {
                                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count();
                                if (elapsed >= DEBOUNCE_MS) {
                                    shouldTrigger = true;
                                    it->second = now;
                                }
                            }
                        }

                        if (shouldTrigger) {
                            try {
                                onFileChanged(fullPath);
                            } catch (const std::exception& e) {
                                std::cerr << "Error in onFileChanged callback: " << e.what() << std::endl;
                            }
                        }
                    }

                    if (fni->NextEntryOffset == 0) {
                        break;
                    }

                    ptr += fni->NextEntryOffset;
                }

                // 重新投递监听
                ZeroMemory(&item.ov, sizeof(item.ov));
                item.ov.hEvent = item.eventHandle;
                startWatch(item);
            }

            // cleanup
            for (auto& i : items) {
                if (i.hDir != INVALID_HANDLE_VALUE) {
                    CloseHandle(i.hDir);
                }
                    
                if (i.eventHandle) {
                    CloseHandle(i.eventHandle);
                }
            }

        });
    }

    // ------------------------------------------------------------

    std::optional<std::thread> watchAndReloadPyFiles(
        const std::vector<std::string_view>& modDirs,
        const std::function<void(const fs::path&)>& onFileChanged
    ) {
        std::vector<fs::path> paths;
        paths.reserve(modDirs.size());

        for (auto sv : modDirs) {
            paths.emplace_back(fs::path(std::string(sv)));
        }

        return watchAndReloadPyFiles(paths, onFileChanged);
    }

    // 监听目标pid进程是否回到前台焦点
    std::optional<std::thread> watchProcessForegroundWindow(
        uint32_t pid,
        const std::function<void(bool isForeground)>& onFocusChanged
    ) {
        return std::thread([pid, onFocusChanged]() {
            HWND lastForegroundWnd = nullptr;
            bool lastIsForeground = false;

            while (true) {
                HWND fgWnd = GetForegroundWindow();
                DWORD fgPid = 0;
                GetWindowThreadProcessId(fgWnd, &fgPid);

                bool isForeground = (fgPid == pid);
                if (isForeground != lastIsForeground) {
                    lastIsForeground = isForeground;
                    try {
                        onFocusChanged(isForeground);
                    } catch (const std::exception& e) {
                        std::cerr << "Error in onFocusChanged callback: " << e.what() << std::endl;
                    }
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
    }

#else // _WIN32

    std::optional<std::thread> watchAndReloadPyFiles(
        const std::vector<std::filesystem::path>&,
        const std::function<void(const std::filesystem::path&)>&
    ) = delete;

    std::optional<std::thread> watchAndReloadPyFiles(
        const std::vector<std::string_view>&,
        const std::function<void(const std::filesystem::path&)>&
    )  = delete;

    std::optional<std::thread> watchProcessForegroundWindow(
        uint32_t,
        const std::function<void(bool isForeground)>&
    ) = delete;

#endif

} // namespace MCDevTool::HotReload
