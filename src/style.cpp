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
#include <objidl.h>
#include <shobjidl.h>
#include "window_capture.h"
#include "window_lookup.h"
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#endif
#include <iostream>
#include <memory>

namespace MCDevTool::Style {
#ifdef _WIN32
    static void hideWindowFromTaskbar(HWND hwnd) {
        const HRESULT initializeResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(initializeResult) && initializeResult != RPC_E_CHANGED_MODE) {
            return;
        }

        ITaskbarList* taskbarList = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbarList)))) {
            if (SUCCEEDED(taskbarList->HrInit())) {
                taskbarList->DeleteTab(hwnd);
            }
            taskbarList->Release();
        }

        if (SUCCEEDED(initializeResult)) {
            CoUninitialize();
        }
    }

    // 应用样式到指定窗口句柄
    static void _applyStyleToMinecraftWindow(HWND hwnd, const StyleConfig& config) {
        // 基础状态
        HWND insertAfter = nullptr;
        UINT baseFlags   = SWP_NOACTIVATE;

        // 当前窗口矩形（物理像素）
        RECT rect{};
        int  x = 0, y = 0, w = 0, h = 0;
        if (GetWindowRect(hwnd, &rect)) {
            x = rect.left;
            y = rect.top;
            w = rect.right - rect.left;
            h = rect.bottom - rect.top;
        }

        // 获取 DPI，用于把“物理像素”配置转换成 Win32 所需单位
        UINT dpi = 96;
        if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
            using GetDpiForWindow_t = UINT(WINAPI*)(HWND);
            auto pGetDpiForWindow   = reinterpret_cast<GetDpiForWindow_t>(GetProcAddress(user32, "GetDpiForWindow"));
            if (pGetDpiForWindow && IsWindow(hwnd)) {
                dpi = pGetDpiForWindow(hwnd);
            } else {
                using GetDpiForSystem_t = UINT(WINAPI*)();
                auto pGetDpiForSystem = reinterpret_cast<GetDpiForSystem_t>(GetProcAddress(user32, "GetDpiForSystem"));
                if (pGetDpiForSystem) {
                    dpi = pGetDpiForSystem();
                }
            }
        }
        const double dpiScale = dpi > 0 ? static_cast<double>(dpi) / 96.0 : 1.0;

        // 置顶控制
        if (config.alwaysOnTop) {
            insertAfter  = HWND_TOPMOST;
            baseFlags   |= SWP_SHOWWINDOW;
        } else {
            insertAfter  = nullptr;
            baseFlags   |= SWP_NOZORDER;
        }

        // 样式 + 尺寸（不移动位置）
        bool needStyle = false;
        bool needSize  = false;
        int  targetW   = w;
        int  targetH   = h;

        // 隐藏标题栏
        if (config.hideTitleBar) {
            LONG style  = GetWindowLongW(hwnd, GWL_STYLE);
            style      &= ~WS_CAPTION;
            SetWindowLongW(hwnd, GWL_STYLE, style);
            needStyle = true;
        }

        if (config.hideTaskbarIcon) {
            hideWindowFromTaskbar(hwnd);
        }

        // 标题栏颜色
        if (config.titleBarColor.has_value()) {
            const auto& color = config.titleBarColor.value();
            COLORREF    captionColor =
                RGB(static_cast<BYTE>(color.red), static_cast<BYTE>(color.green), static_cast<BYTE>(color.blue));
            DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));
        }

        // 窗口整体不透明度（包括客户区和标题栏）
        if (config.windowOpacity.has_value()) {
            LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
            if ((exStyle & WS_EX_LAYERED) == 0) {
                SetWindowLongW(hwnd, GWL_EXSTYLE, exStyle | WS_EX_LAYERED);
            }
            SetLayeredWindowAttributes(hwnd, 0, static_cast<BYTE>(config.windowOpacity.value()), LWA_ALPHA);
        }

        // 固定大小：配置视为“客户区物理像素”，按 DPI 反推需要设置的窗口外框尺寸
        if (config.fixedSize.has_value()) {
            const auto& size = config.fixedSize.value();
            // 物理客户区 -> 逻辑客户区
            int clientLogicalW = static_cast<int>(size.width / dpiScale + 0.5);
            int clientLogicalH = static_cast<int>(size.height / dpiScale + 0.5);

            LONG style   = GetWindowLongW(hwnd, GWL_STYLE);
            LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);
            RECT rc      = {0, 0, clientLogicalW, clientLogicalH};
            if (AdjustWindowRectEx(&rc, static_cast<DWORD>(style), FALSE, static_cast<DWORD>(exStyle))) {
                targetW = rc.right - rc.left;
                targetH = rc.bottom - rc.top;
            } else {
                targetW = clientLogicalW;
                targetH = clientLogicalH;
            }
            needSize = true;
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
        int  targetX  = x;
        int  targetY  = y;

        // 固定位置
        if (config.fixedPosition.has_value()) {
            const auto& position = config.fixedPosition.value();
            targetX              = position.x;
            targetY              = position.y;
            needMove             = true;
        }

        // 锁定角落（优先级高于 fixedPosition）
        if (config.lockCorner.has_value()) {
            RECT workArea{};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
            int workWidth  = workArea.right - workArea.left;
            int workHeight = workArea.bottom - workArea.top;
            int workLeft   = workArea.left;
            int workTop    = workArea.top;

            switch (config.lockCorner.value()) {
            case WindowCorner::TopLeft:
                targetX = workLeft;
                targetY = workTop;
                break;
            case WindowCorner::TopRight:
                targetX = workLeft + workWidth - w;
                targetY = workTop;
                break;
            case WindowCorner::BottomLeft:
                targetX = workLeft;
                targetY = workTop + workHeight - h;
                break;
            case WindowCorner::BottomRight:
                targetX = workLeft + workWidth - w;
                targetY = workTop + workHeight - h;
                break;
            default:
                break;
            }

            // 保证不超出工作区
            if (targetX < workLeft) targetX = workLeft;
            if (targetY < workTop) targetY = workTop;
            if (targetX + w > workArea.right) targetX = workArea.right - w;
            if (targetY + h > workArea.bottom) targetY = workArea.bottom - h;

            needMove = true;
        }

        if (needMove) {
            UINT flags = baseFlags | SWP_NOSIZE;
            SetWindowPos(hwnd, insertAfter, targetX, targetY, w, h, flags);
        }
    }

    // 应用样式到指定 PID 的 Minecraft 窗口
    bool applyStyleToMinecraftWindow(int pid, const StyleConfig& config) {
        HWND hwnd = MCDevTool::Detail::findMinecraftWindow(static_cast<DWORD>(pid));
        if (!hwnd) {
            return false; // 未找到窗口
        }
        _applyStyleToMinecraftWindow(hwnd, config);
        return true;
    }

#else
    bool applyStyleToMinecraftWindow(int pid, const StyleConfig& config) = delete;
#endif

    MinecraftWindowStyler::MinecraftWindowStyler(int pid, const StyleConfig& config) : mPid(pid), mConfig(config) {}

    MinecraftWindowStyler::MinecraftWindowStyler(int pid, StyleConfig&& config)
    : mPid(pid),
      mConfig(std::move(config)) {}

    MinecraftWindowStyler::MinecraftWindowStyler(int pid) : mPid(pid) {}

    MinecraftWindowStyler::~MinecraftWindowStyler() { safeExit(); }

    void MinecraftWindowStyler::start() {
        if (mThread.has_value() || mPid == 0) {
            return;
        }
        mStopFlag = false;
        mThread   = std::thread([this]() {
            unsigned int tick = 0;
            while (!mStopFlag.load()) {
                if (tick % 10 == 0 && applyStyleToMinecraftWindow(mPid, mConfig)) {
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

    void MinecraftWindowStyler::setPid(int pid) { mPid = pid; }

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

    // 根据指定pid获取窗口内的画面信息 返回压缩480p的jpg数据
    std::optional<std::vector<uint8_t>> captureMinecraftWindow480p(int pid) {
#ifdef _WIN32
        HWND hwnd = MCDevTool::Detail::findMinecraftWindow(static_cast<DWORD>(pid));
        if (!hwnd || IsIconic(hwnd)) {
            return std::nullopt;
        }
        return Detail::captureWindow480p(hwnd);
#else
        (void)pid;
        return std::nullopt;
#endif
    }

    bool triggerMinecraftUiReloadShortcut(int pid) {
#ifdef _WIN32
        HWND hwnd = MCDevTool::Detail::findMinecraftWindow(static_cast<DWORD>(pid));
        if (!hwnd) {
            return false;
        }

        if (IsIconic(hwnd)) {
            return false;
        }

        auto makeKeyLParam = [](UINT vk, bool keyUp) -> LPARAM {
            UINT   scan   = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
            LPARAM lParam = 1 | (static_cast<LPARAM>(scan) << 16);
            if (keyUp) {
                lParam |= (1LL << 30) | (1LL << 31);
            }
            return lParam;
        };

        const bool ok = PostMessageW(hwnd, WM_KEYDOWN, VK_CONTROL, makeKeyLParam(VK_CONTROL, false))
                     && PostMessageW(hwnd, WM_KEYDOWN, 'R', makeKeyLParam('R', false))
                     && PostMessageW(hwnd, WM_KEYUP, 'R', makeKeyLParam('R', true))
                     && PostMessageW(hwnd, WM_KEYUP, VK_CONTROL, makeKeyLParam(VK_CONTROL, true));
        return ok;
#else
        (void)pid;
        return false;
#endif
    }
} // namespace MCDevTool::Style
