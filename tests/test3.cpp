#include <iostream>
#include <string>
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

// Windows 11 22H2+ 支持的属性
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif

// Windows 11 背景类型
// enum DWM_SYSTEMBACKDROP_TYPE {
//     DWMSBT_AUTO = 0,            // 自动（跟随系统）
//     DWMSBT_NONE = 1,            // 无效果
//     DWMSBT_MAINWINDOW = 2,      // Mica 效果
//     DWMSBT_TRANSIENTWINDOW = 3, // Acrylic 效果（高斯模糊）
//     DWMSBT_TABBEDWINDOW = 4     // Tabbed Mica 效果
// };

// 根据进程 ID 和窗口标题关键字，使符合条件的窗口置顶悬浮
static bool makeTopMostByPidAndTitleContains(
    DWORD pid,
    const std::wstring& keyword,
    bool setDarkTitleBar = true
) {
    struct Context {
        DWORD pid;
        const std::wstring* keyword;
        HWND found = nullptr;
    } ctx { pid, &keyword, nullptr };

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto& ctx = *(Context*)lParam;

        // 必须是可见顶层
        if (!IsWindowVisible(hwnd))
            return TRUE;
        if (GetWindow(hwnd, GW_OWNER) != nullptr)
            return TRUE;

        // 如果指定 PID 先过滤
        if (ctx.pid != 0) {
            DWORD winPid = 0;
            GetWindowThreadProcessId(hwnd, &winPid);
            if (winPid != ctx.pid)
                return TRUE;
        }

        // 获取标题
        wchar_t title[512];
        int len = GetWindowTextW(hwnd, title, 512);
        if (len == 0)
            return TRUE;

        std::wstring windowTitle = title;

        // 检查是否包含关键字
        if (windowTitle.find(*ctx.keyword) == std::wstring::npos)
            return TRUE;

        ctx.found = hwnd;
        return FALSE; // 终止搜索
    }, (LPARAM)&ctx);

    if (!ctx.found) {
        return false; // 未找到
    }

    HWND hwnd = ctx.found;

    // 设置标题栏为黑色（暗色模式）
    if (setDarkTitleBar) {
        // 启用暗色模式标题栏（Windows 10 1809+ / Windows 11）
        BOOL useDarkMode = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));

        // Windows 11 22H2+：启用 Acrylic 高斯模糊效果
        // DWMSBT_TRANSIENTWINDOW = Acrylic（高斯模糊，混合背景）
        // DWMSBT_MAINWINDOW = Mica（更柔和，性能更好）
        // DWMSBT_TABBEDWINDOW = Tabbed Mica
        int backdropType = DWMSBT_TRANSIENTWINDOW;  // Acrylic 高斯模糊
        HRESULT hr = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));
        
        if (FAILED(hr)) {
            // 如果 Acrylic 失败，回退到纯色
            COLORREF captionColor = RGB(0x1F, 0x1F, 0x1F);
            DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));
        }

        // 设置标题栏文字颜色为浅灰白色
        COLORREF textColor = RGB(0xCC, 0xCC, 0xCC);
        DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &textColor, sizeof(textColor));
    }

    // 设置置顶悬浮属性
    LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    ex |= WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    ex &= ~WS_EX_APPWINDOW;
    SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex);

    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);

    return true;
}


// 根据进程 ID 和窗口标题关键字，使符合条件的窗口置顶悬浮
// static bool makeTopMostByPidAndTitleContains(
//     DWORD pid,
//     const std::wstring& keyword,
//     bool setDarkTitleBar = true
// ) {
//     struct Context {
//         DWORD pid;
//         const std::wstring* keyword;
//         HWND found = nullptr;
//     } ctx { pid, &keyword, nullptr };

//     EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
//         auto& ctx = *(Context*)lParam;

//         // 必须是可见顶层
//         if (!IsWindowVisible(hwnd))
//             return TRUE;
//         if (GetWindow(hwnd, GW_OWNER) != nullptr)
//             return TRUE;

//         // 如果指定 PID 先过滤
//         if (ctx.pid != 0) {
//             DWORD winPid = 0;
//             GetWindowThreadProcessId(hwnd, &winPid);
//             if (winPid != ctx.pid)
//                 return TRUE;
//         }

//         // 获取标题
//         wchar_t title[512];
//         int len = GetWindowTextW(hwnd, title, 512);
//         if (len == 0)
//             return TRUE;

//         std::wstring windowTitle = title;

//         // 检查是否包含关键字
//         if (windowTitle.find(*ctx.keyword) == std::wstring::npos)
//             return TRUE;

//         ctx.found = hwnd;
//         return FALSE; // 终止搜索
//     }, (LPARAM)&ctx);

//     if (!ctx.found) {
//         return false; // 未找到
//     }

//     HWND hwnd = ctx.found;

//     // 设置标题栏为黑色（暗色模式）
//     if (setDarkTitleBar) {
//         // 启用暗色模式标题栏（Windows 10 1809+ / Windows 11）
//         BOOL useDarkMode = TRUE;
//         DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDarkMode, sizeof(useDarkMode));

//         // Windows 11 22H2+ 可以直接设置标题栏颜色
//         // VS Code 风格的灰黑色 (#1F1F1F / #252526 / #323233)
//         COLORREF captionColor = RGB(0x1F, 0x1F, 0x1F);  // VS Code 标题栏颜色
//         DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));

//         // 可选：设置标题栏文字颜色为浅灰白色
//         COLORREF textColor = RGB(0xCC, 0xCC, 0xCC);  // VS Code 风格的浅灰色文字
//         DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &textColor, sizeof(textColor));
//     }

//     // 设置置顶悬浮属性
//     LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
//     ex |= WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
//     ex &= ~WS_EX_APPWINDOW;
//     SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex);

//     SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
//                  SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);

//     return true;
// }


int main() {
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

    makeTopMostByPidAndTitleContains(0, L"Minecraft");

    return 0;
}