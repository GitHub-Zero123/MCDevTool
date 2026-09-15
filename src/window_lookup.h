#pragma once

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string_view>

namespace MCDevTool::Detail {
    // 按 pid + 标题关键字定位顶层窗口。截图、样式与输入注入共用同一份解析规则，
    // 避免三条路径各自找到不同的窗口。
    inline HWND findWindowByPidAndTitleContains(DWORD pid, std::wstring_view keyword) {
        struct Context {
            DWORD             pid;
            std::wstring_view keyword;
            HWND              found = nullptr;
        } context{pid, keyword, nullptr};

        EnumWindows(
            [](HWND hwnd, LPARAM lParam) -> BOOL {
                auto& context = *reinterpret_cast<Context*>(lParam);

                if (!IsWindowVisible(hwnd)) return TRUE;
                if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;

                if (context.pid != 0) {
                    DWORD windowPid = 0;
                    GetWindowThreadProcessId(hwnd, &windowPid);
                    if (windowPid != context.pid) return TRUE;
                }

                wchar_t   title[512];
                const int length = GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
                if (length == 0) return TRUE;
                if (std::wstring_view{title, static_cast<std::size_t>(length)}.find(context.keyword)
                    == std::wstring_view::npos) {
                    return TRUE;
                }

                context.found = hwnd;
                return FALSE; // 找到了就停止枚举
            },
            reinterpret_cast<LPARAM>(&context)
        );

        return context.found; // 未找到时为 nullptr
    }

    inline HWND findMinecraftWindow(DWORD pid) { return findWindowByPidAndTitleContains(pid, L"Minecraft"); }
} // namespace MCDevTool::Detail
#endif
