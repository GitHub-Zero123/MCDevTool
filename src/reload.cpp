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

            if (!startWatch(item)) {
                CloseHandle(item.hDir);
                CloseHandle(item.eventHandle);
                continue;
            }

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

                if (index == WAIT_FAILED) {
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

                    // 仅处理文件修改事件
                    if (fni->Action == FILE_ACTION_MODIFIED) {
                        std::wstring name(
                            fni->FileName,
                            fni->FileNameLength / sizeof(wchar_t)
                        );

                        fs::path fullPath = item.dir / name;

                        if (fullPath.extension() == L".py") {
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

#endif // _WIN32

} // namespace MCDevTool::HotReload
