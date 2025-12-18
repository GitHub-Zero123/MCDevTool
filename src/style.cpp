#include "mcdevtool/style.h"
#include <optional>
#include <vector>
#include <string>
#include <thread>
#include <atomic>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif
#include <iostream>

namespace MCDevTool::Style {
#ifdef _WIN32
    // 根据进程ID关键字搜索指定窗口
    static HWND findWindowByPidAndTitleContains(DWORD pid, const std::wstring& keyword) {
        struct Context {
            DWORD pid;
            const std::wstring* keyword;
            HWND found = nullptr;
        } ctx{ pid, &keyword, nullptr };

        EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
            auto& ctx = *(Context*)lParam;

            if (!IsWindowVisible(hwnd))
                return TRUE;
            if (GetWindow(hwnd, GW_OWNER) != nullptr)
                return TRUE;

            // PID 过滤
            if (ctx.pid != 0) {
                DWORD winPid = 0;
                GetWindowThreadProcessId(hwnd, &winPid);
                if (winPid != ctx.pid)
                    return TRUE;
            }

            // 标题过滤
            wchar_t buf[512];
            int len = GetWindowTextW(hwnd, buf, 512);
            if (len == 0)
                return TRUE;

            std::wstring title = buf;
            if (title.find(*ctx.keyword) == std::wstring::npos)
                return TRUE;

            ctx.found = hwnd;
            return FALSE; // 找到了就停止枚举
        }, (LPARAM)&ctx);

        return ctx.found; // nullptr
    }

    // 应用样式到指定窗口句柄
    static void _applyStyleToMinecraftWindow(HWND hwnd, const StyleConfig& config) {
        // 基础状态
        HWND insertAfter = nullptr;
        UINT baseFlags = SWP_NOACTIVATE;

        // 当前窗口矩形（物理像素）
        RECT rect{};
        int x = 0, y = 0, w = 0, h = 0;
        if (GetWindowRect(hwnd, &rect)) {
            x = rect.left;
            y = rect.top;
            w = rect.right - rect.left;
            h = rect.bottom - rect.top;
        }

        // 获取 DPI，用于把“物理像素”配置转换成 Win32 所需单位
        UINT dpi = 96;
        if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
            using GetDpiForWindow_t = UINT (WINAPI*)(HWND);
            auto pGetDpiForWindow = reinterpret_cast<GetDpiForWindow_t>(
                GetProcAddress(user32, "GetDpiForWindow")
            );
            if (pGetDpiForWindow && IsWindow(hwnd)) {
                dpi = pGetDpiForWindow(hwnd);
            } else {
                using GetDpiForSystem_t = UINT (WINAPI*)();
                auto pGetDpiForSystem = reinterpret_cast<GetDpiForSystem_t>(
                    GetProcAddress(user32, "GetDpiForSystem")
                );
                if (pGetDpiForSystem) {
                    dpi = pGetDpiForSystem();
                }
            }
        }
        const double dpiScale = dpi > 0 ? static_cast<double>(dpi) / 96.0 : 1.0;

        // 置顶控制
        if (config.alwaysOnTop) {
            insertAfter = HWND_TOPMOST;
            baseFlags |= SWP_SHOWWINDOW;
        } else {
            insertAfter = nullptr;
            baseFlags |= SWP_NOZORDER;
        }

        // 样式 + 尺寸（不移动位置）
        bool needStyle = false;
        bool needSize = false;
        int targetW = w;
        int targetH = h;

        // 隐藏标题栏
        if (config.hideTitleBar) {
            LONG style = GetWindowLongW(hwnd, GWL_STYLE);
            style &= ~WS_CAPTION;
            SetWindowLongW(hwnd, GWL_STYLE, style);
            needStyle = true;
        }

        // 标题栏颜色
        if (config.titleBarColor.has_value()) {
            const auto& colorVec = config.titleBarColor.value();
            if (colorVec.size() >= 3) {
                COLORREF captionColor = RGB(
                    static_cast<BYTE>(colorVec[0]),
                    static_cast<BYTE>(colorVec[1]),
                    static_cast<BYTE>(colorVec[2])
                );
                DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));
            }
        }

        // 固定大小：配置视为“客户区物理像素”，按 DPI 反推需要设置的窗口外框尺寸
        if (config.fixedSize.has_value()) {
            const auto& sizeVec = config.fixedSize.value();
            if (sizeVec.size() >= 2) {
                int clientPhysicalW = sizeVec[0];
                int clientPhysicalH = sizeVec[1];

                // 物理客户区 -> 逻辑客户区
                int clientLogicalW = static_cast<int>(clientPhysicalW / dpiScale + 0.5);
                int clientLogicalH = static_cast<int>(clientPhysicalH / dpiScale + 0.5);

                LONG style = GetWindowLongW(hwnd, GWL_STYLE);
                LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
                RECT rc = { 0, 0, clientLogicalW, clientLogicalH };
                if (AdjustWindowRectEx(&rc, static_cast<DWORD>(style), FALSE, static_cast<DWORD>(exStyle))) {
                    targetW = rc.right - rc.left;
                    targetH = rc.bottom - rc.top;
                } else {
                    targetW = clientLogicalW;
                    targetH = clientLogicalH;
                }
                needSize = true;
            }
        }

        if (needStyle || needSize || config.alwaysOnTop) {
            UINT flags = baseFlags | SWP_NOMOVE;
            if (!needSize) {
                flags |= SWP_NOSIZE;
            }
            if (needStyle) {
                flags |= SWP_FRAMECHANGED;
            }
            SetWindowPos(hwnd, insertAfter, x, y, targetW, targetH, flags);

            // 应用完样式和尺寸后，再获取一次实际窗口大小
            if (GetWindowRect(hwnd, &rect)) {
                x = rect.left;
                y = rect.top;
                w = rect.right - rect.left;
                h = rect.bottom - rect.top;
            }
        }

        // 位置（fixed_position / lock_corner）
        bool needMove = false;
        int targetX = x;
        int targetY = y;

        // 固定位置
        if (config.fixedPosition.has_value()) {
            const auto& posVec = config.fixedPosition.value();
            if (posVec.size() >= 2) {
                targetX = posVec[0];
                targetY = posVec[1];
                needMove = true;
            }
        }

        // 锁定角落（优先级高于 fixedPosition）
        if (config.lockCorner.has_value()) {
            RECT workArea{};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
            int workWidth = workArea.right - workArea.left;
            int workHeight = workArea.bottom - workArea.top;
            int workLeft = workArea.left;
            int workTop = workArea.top;

            switch (config.lockCorner.value()) {
                case 1: // 左上
                    targetX = workLeft;
                    targetY = workTop;
                    break;
                case 2: // 右上
                    targetX = workLeft + workWidth - w;
                    targetY = workTop;
                    break;
                case 3: // 左下
                    targetX = workLeft;
                    targetY = workTop + workHeight - h;
                    break;
                case 4: // 右下
                    targetX = workLeft + workWidth - w;
                    targetY = workTop + workHeight - h;
                    break;
                default:
                    break;
            }

            // 保证不超出工作区
            if (targetX < workLeft) targetX = workLeft;
            if (targetY < workTop)  targetY = workTop;
            if (targetX + w > workArea.right)  targetX = workArea.right  - w;
            if (targetY + h > workArea.bottom) targetY = workArea.bottom - h;

            needMove = true;
        }

        if (needMove) {
            UINT flags = baseFlags | SWP_NOSIZE;
            SetWindowPos(hwnd, insertAfter, targetX, targetY, w, h, flags);
        }
    }

    // 应用样式到指定 PID 的 Minecraft 窗口
    bool applyStyleToMinecraftWindow(
        int pid,
        const StyleConfig& config
    ) {
        static const std::wstring windowTitleKeyword = L"Minecraft";
        HWND hwnd = findWindowByPidAndTitleContains(static_cast<DWORD>(pid), windowTitleKeyword);
        if (!hwnd) {
            return false; // 未找到窗口
        }
        _applyStyleToMinecraftWindow(hwnd, config);
        return true;
    }

#else
    bool applyStyleToMinecraftWindow(
        int pid,
        const StyleConfig& config
    ) = delete;
#endif

    MinecraftWindowStyler::MinecraftWindowStyler(int pid, const StyleConfig& config)
        : mPid(pid), mConfig(config) {
    }

    MinecraftWindowStyler::MinecraftWindowStyler(int pid, StyleConfig&& config)
        : mPid(pid), mConfig(std::move(config)) {
    }

    MinecraftWindowStyler::MinecraftWindowStyler(int pid)
        : mPid(pid) {
    }

    void MinecraftWindowStyler::start() {
        if (mThread.has_value() || mPid == 0) {
            return;
        }
        mStopFlag = false;
        mThread = std::thread([this]() {
            unsigned int tick = 0;
            while (!mStopFlag.load()) {
                if(tick % 10 == 0 && applyStyleToMinecraftWindow(mPid, mConfig)) {
                    // 成功应用样式
                    onStyleApplied();
                    return;
                }
                tick++;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
    }

    void MinecraftWindowStyler::onStyleApplied() {}

    void MinecraftWindowStyler::setPid(int pid) {
        mPid = pid;
    }

    void MinecraftWindowStyler::safeExit() {
        mStopFlag = true;
        join();
    }

    void MinecraftWindowStyler::join() {
        if (mThread.has_value() && mThread->joinable()) {
            mThread->join();
            mThread.reset();
        }
    }
} // namespace MCDevTool::Style
