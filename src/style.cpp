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
        // 收集 SetWindowPos 参数
        bool needUpdatePos = false;
        bool needUpdateStyle = false;
        HWND insertAfter = nullptr;
        UINT swpFlags = SWP_NOACTIVATE;
        int x = 0, y = 0, w = 0, h = 0;

        // 获取当前窗口位置和大小
        RECT rect{};
        if (GetWindowRect(hwnd, &rect)) {
            x = rect.left;
            y = rect.top;
            w = rect.right - rect.left;
            h = rect.bottom - rect.top;
        }

        // 置顶
        if (config.alwaysOnTop) {
            insertAfter = HWND_TOPMOST;
            swpFlags |= SWP_SHOWWINDOW;
            needUpdatePos = true;
        } else {
            swpFlags |= SWP_NOZORDER;
        }

        // 隐藏标题栏
        if (config.hideTitleBar) {
            LONG style = GetWindowLongW(hwnd, GWL_STYLE);
            style &= ~WS_CAPTION;
            SetWindowLongW(hwnd, GWL_STYLE, style);
            swpFlags |= SWP_FRAMECHANGED;
            needUpdateStyle = true;
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

        // 固定大小
        if (config.fixedSize.has_value()) {
            const auto& sizeVec = config.fixedSize.value();
            if (sizeVec.size() >= 2) {
                w = sizeVec[0];
                h = sizeVec[1];
                needUpdatePos = true;
            }
        }

        // 固定位置
        if (config.fixedPosition.has_value()) {
            const auto& posVec = config.fixedPosition.value();
            if (posVec.size() >= 2) {
                x = posVec[0];
                y = posVec[1];
                needUpdatePos = true;
            }
        }

        // 锁定角落（优先级高于 fixedPosition）
        if (config.lockCorner.has_value()) {
            // 获取工作区域（排除任务栏）
            RECT workArea{};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
            int workWidth = workArea.right - workArea.left;
            int workHeight = workArea.bottom - workArea.top;
            int workLeft = workArea.left;
            int workTop = workArea.top;

            switch (config.lockCorner.value()) {
                case 1: // 左上
                    x = workLeft;
                    y = workTop;
                    break;
                case 2: // 右上
                    x = workLeft + workWidth - w;
                    y = workTop;
                    break;
                case 3: // 左下
                    x = workLeft;
                    y = workTop + workHeight - h;
                    break;
                case 4: // 右下
                    x = workLeft + workWidth - w;
                    y = workTop + workHeight - h;
                    break;
                default:
                    break;
            }
            needUpdatePos = true;
        }

        // 一次性应用 SetWindowPos
        if (needUpdatePos || needUpdateStyle) {
            if (!needUpdatePos) {
                // 仅样式更新，不改变位置和大小
                swpFlags |= SWP_NOMOVE | SWP_NOSIZE;
            }
            SetWindowPos(hwnd, insertAfter, x, y, w, h, swpFlags);
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
