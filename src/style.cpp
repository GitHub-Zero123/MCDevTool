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
#include <gdiplus.h>
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#endif
#include <iostream>
#include <memory>

namespace MCDevTool::Style {
#ifdef _WIN32
    // 根据进程ID关键字搜索指定窗口
    static HWND findWindowByPidAndTitleContains(DWORD pid, const std::wstring& keyword) {
        struct Context {
            DWORD               pid;
            const std::wstring* keyword;
            HWND                found = nullptr;
        } ctx{pid, &keyword, nullptr};

        EnumWindows(
            [](HWND hwnd, LPARAM lParam) -> BOOL {
                auto& ctx = *(Context*)lParam;

                if (!IsWindowVisible(hwnd)) return TRUE;
                if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;

                // PID 过滤
                if (ctx.pid != 0) {
                    DWORD winPid = 0;
                    GetWindowThreadProcessId(hwnd, &winPid);
                    if (winPid != ctx.pid) return TRUE;
                }

                // 标题过滤
                wchar_t buf[512];
                int     len = GetWindowTextW(hwnd, buf, 512);
                if (len == 0) return TRUE;

                std::wstring title = buf;
                if (title.find(*ctx.keyword) == std::wstring::npos) return TRUE;

                ctx.found = hwnd;
                return FALSE; // 找到了就停止枚举
            },
            (LPARAM)&ctx
        );

        return ctx.found; // nullptr
    }

    static void hideWindowFromTaskbar(HWND hwnd) {
        const HRESULT initializeResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(initializeResult) && initializeResult != RPC_E_CHANGED_MODE) {
            return;
        }

        ITaskbarList* taskbarList = nullptr;
        if (SUCCEEDED(CoCreateInstance(
                CLSID_TaskbarList,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&taskbarList)
            ))) {
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
            const auto& colorVec = config.titleBarColor.value();
            if (colorVec.size() >= 3) {
                COLORREF captionColor =
                    RGB(static_cast<BYTE>(colorVec[0]), static_cast<BYTE>(colorVec[1]), static_cast<BYTE>(colorVec[2]));
                DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));
            }
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
            const auto& sizeVec = config.fixedSize.value();
            if (sizeVec.size() >= 2) {
                int clientPhysicalW = sizeVec[0];
                int clientPhysicalH = sizeVec[1];

                // 物理客户区 -> 逻辑客户区
                int clientLogicalW = static_cast<int>(clientPhysicalW / dpiScale + 0.5);
                int clientLogicalH = static_cast<int>(clientPhysicalH / dpiScale + 0.5);

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
            const auto& posVec = config.fixedPosition.value();
            if (posVec.size() >= 2) {
                targetX  = posVec[0];
                targetY  = posVec[1];
                needMove = true;
            }
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
        static const std::wstring windowTitleKeyword = L"Minecraft";
        HWND                      hwnd = findWindowByPidAndTitleContains(static_cast<DWORD>(pid), windowTitleKeyword);
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
        // 延迟初始化 GDI+ (线程安全, 仅一次)
        static ULONG_PTR s_gdipToken = []() {
            ULONG_PTR                    token = 0;
            Gdiplus::GdiplusStartupInput input;
            Gdiplus::GdiplusStartup(&token, &input, nullptr);
            return token;
        }();
        (void)s_gdipToken;

        // 查找 Minecraft 窗口
        static const std::wstring keyword = L"Minecraft";
        HWND                      hwnd    = findWindowByPidAndTitleContains(static_cast<DWORD>(pid), keyword);
        if (!hwnd || IsIconic(hwnd)) {
            return std::nullopt; // 窗口不存在或最小化
        }

        // ---- 临时切换到 Per-Monitor DPI 感知，获取物理像素尺寸 ----
        // 避免 DPI 虚拟化导致 GetWindowRect/GetClientRect 返回缩放后的逻辑坐标
        using SetThreadDpiAwarenessContext_t      = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
        SetThreadDpiAwarenessContext_t pSetDpiCtx = nullptr;
        DPI_AWARENESS_CONTEXT          oldDpiCtx  = nullptr;

        if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
            pSetDpiCtx = reinterpret_cast<SetThreadDpiAwarenessContext_t>(
                GetProcAddress(user32, "SetThreadDpiAwarenessContext")
            );
        }
        if (pSetDpiCtx) {
            oldDpiCtx = pSetDpiCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }

        // 获取客户区尺寸（物理像素）
        RECT clientRect{};
        if (!GetClientRect(hwnd, &clientRect)) {
            if (pSetDpiCtx && oldDpiCtx) pSetDpiCtx(oldDpiCtx);
            return std::nullopt;
        }
        int srcW = clientRect.right;
        int srcH = clientRect.bottom;
        if (srcW <= 0 || srcH <= 0) {
            if (pSetDpiCtx && oldDpiCtx) pSetDpiCtx(oldDpiCtx);
            return std::nullopt;
        }

        // 获取窗口整体尺寸（物理像素, PrintWindow 需要）
        RECT windowRect{};
        GetWindowRect(hwnd, &windowRect);
        int winW = windowRect.right - windowRect.left;
        int winH = windowRect.bottom - windowRect.top;

        // 计算客户区在窗口中的偏移（物理像素）
        POINT clientOrigin = {0, 0};
        ClientToScreen(hwnd, &clientOrigin);
        int offsetX = clientOrigin.x - windowRect.left;
        int offsetY = clientOrigin.y - windowRect.top;

        // 恢复原 DPI 感知上下文
        if (pSetDpiCtx && oldDpiCtx) {
            pSetDpiCtx(oldDpiCtx);
        }

        // --- 使用屏幕 DC 以确保位图使用物理像素分辨率 ---
        HDC hdcScreen = GetDC(nullptr);
        if (!hdcScreen) return std::nullopt;

        // 1. 捕获整个窗口（物理像素尺寸）
        HDC     hdcCapture = CreateCompatibleDC(hdcScreen);
        HBITMAP hCapBmp    = CreateCompatibleBitmap(hdcScreen, winW, winH);
        HGDIOBJ hOldCap    = SelectObject(hdcCapture, hCapBmp);

        // PrintWindow: PW_RENDERFULLCONTENT(0x02) 支持窗口被遮挡时也能捕获
        if (!PrintWindow(hwnd, hdcCapture, 0x02)) {
            // 回退到 BitBlt（窗口被遮挡时可能无法获取完整画面）
            HDC hdcWindow = GetDC(hwnd);
            if (hdcWindow) {
                BitBlt(hdcCapture, 0, 0, winW, winH, hdcWindow, 0, 0, SRCCOPY);
                ReleaseDC(hwnd, hdcWindow);
            }
        }

        // 2. 计算目标 480p 尺寸 (基于客户区保持宽高比)
        constexpr int TARGET_H = 480;
        int           dstW, dstH;
        if (srcH <= TARGET_H) {
            dstW = srcW;
            dstH = srcH;
        } else {
            double scale = static_cast<double>(TARGET_H) / srcH;
            dstW         = static_cast<int>(srcW * scale + 0.5);
            dstH         = TARGET_H;
        }

        // 3. 从捕获位图中裁剪客户区 -> 缩放到目标尺寸
        HDC     hdcScaled = CreateCompatibleDC(hdcScreen);
        HBITMAP hScaleBmp = CreateCompatibleBitmap(hdcScreen, dstW, dstH);
        HGDIOBJ hOldScale = SelectObject(hdcScaled, hScaleBmp);

        SetStretchBltMode(hdcScaled, HALFTONE);
        SetBrushOrgEx(hdcScaled, 0, 0, nullptr);
        StretchBlt(hdcScaled, 0, 0, dstW, dstH, hdcCapture, offsetX, offsetY, srcW, srcH, SRCCOPY);

        // GDI+ Bitmap 从 HBITMAP 构建
        Gdiplus::Bitmap bitmap(hScaleBmp, nullptr);

        // 查找 JPEG 编码器 CLSID
        CLSID jpegClsid{};
        bool  foundEncoder = false;
        {
            UINT num = 0, size = 0;
            Gdiplus::GetImageEncodersSize(&num, &size);
            if (size > 0) {
                auto buf    = std::make_unique<uint8_t[]>(size);
                auto codecs = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.get());
                Gdiplus::GetImageEncoders(num, size, codecs);
                for (UINT i = 0; i < num; i++) {
                    if (wcscmp(codecs[i].MimeType, L"image/jpeg") == 0) {
                        jpegClsid    = codecs[i].Clsid;
                        foundEncoder = true;
                        break;
                    }
                }
            }
        }

        std::optional<std::vector<uint8_t>> result = std::nullopt;

        if (foundEncoder) {
            // JPEG 质量参数
            Gdiplus::EncoderParameters params;
            ULONG                      quality = 75;
            params.Count                       = 1;
            params.Parameter[0].Guid           = Gdiplus::EncoderQuality;
            params.Parameter[0].Type           = Gdiplus::EncoderParameterValueTypeLong;
            params.Parameter[0].NumberOfValues = 1;
            params.Parameter[0].Value          = &quality;

            // 编码到内存流
            IStream* pStream = nullptr;
            if (SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &pStream)) && pStream) {
                if (bitmap.Save(pStream, &jpegClsid, &params) == Gdiplus::Ok) {
                    // 读取流数据
                    STATSTG stat{};
                    pStream->Stat(&stat, STATFLAG_NONAME);
                    ULONG dataSize = static_cast<ULONG>(stat.cbSize.QuadPart);
                    if (dataSize > 0) {
                        std::vector<uint8_t> jpegData(dataSize);
                        LARGE_INTEGER        li{};
                        pStream->Seek(li, STREAM_SEEK_SET, nullptr);
                        ULONG bytesRead = 0;
                        pStream->Read(jpegData.data(), dataSize, &bytesRead);
                        result = std::move(jpegData);
                    }
                }
                pStream->Release();
            }
        }

        // 清理 GDI 资源
        SelectObject(hdcCapture, hOldCap);
        DeleteObject(hCapBmp);
        DeleteDC(hdcCapture);
        SelectObject(hdcScaled, hOldScale);
        DeleteObject(hScaleBmp);
        DeleteDC(hdcScaled);
        ReleaseDC(nullptr, hdcScreen);

        return result;
#else
        (void)pid;
        return std::nullopt;
#endif
    }

    // 根据指定pid点击窗口画面的指定坐标（百分比为单位确保适配不同分辨率）点击坐标为(0.0-1.0, 0.0-1.0)
    bool clickMinecraftWindowAt(int pid, double xPercent, double yPercent) {
#ifdef _WIN32
        if (xPercent < 0.0 || xPercent > 1.0 || yPercent < 0.0 || yPercent > 1.0) {
            return false;
        }

        // 查找 Minecraft 窗口
        static const std::wstring keyword = L"Minecraft";
        HWND                      hwnd    = findWindowByPidAndTitleContains(static_cast<DWORD>(pid), keyword);
        if (!hwnd) {
            return false;
        }

        // 如果窗口最小化则先还原
        if (IsIconic(hwnd)) {
            ShowWindow(hwnd, SW_RESTORE);
            Sleep(100);
        }

        // 强制将窗口拉到前台（SendInput 只对前台窗口有效）
        {
            DWORD foreThread   = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
            DWORD targetThread = GetWindowThreadProcessId(hwnd, nullptr);
            if (foreThread != targetThread) {
                AttachThreadInput(foreThread, targetThread, TRUE);
                SetForegroundWindow(hwnd);
                BringWindowToTop(hwnd);
                AttachThreadInput(foreThread, targetThread, FALSE);
            } else {
                SetForegroundWindow(hwnd);
            }
            // 等待窗口实际到达前台
            for (int i = 0; i < 20; ++i) {
                if (GetForegroundWindow() == hwnd) break;
                Sleep(10);
            }
        }

        // 临时切换 DPI 感知，获取物理像素客户区尺寸（与 captureMinecraftWindow480p 一致）
        using SetThreadDpiAwarenessContext_t      = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
        SetThreadDpiAwarenessContext_t pSetDpiCtx = nullptr;
        DPI_AWARENESS_CONTEXT          oldDpiCtx  = nullptr;

        if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
            pSetDpiCtx = reinterpret_cast<SetThreadDpiAwarenessContext_t>(
                GetProcAddress(user32, "SetThreadDpiAwarenessContext")
            );
        }
        if (pSetDpiCtx) {
            oldDpiCtx = pSetDpiCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }

        // 获取客户区物理像素尺寸
        RECT clientRect{};
        if (!GetClientRect(hwnd, &clientRect)) {
            if (pSetDpiCtx && oldDpiCtx) pSetDpiCtx(oldDpiCtx);
            return false;
        }
        int clientW = clientRect.right;
        int clientH = clientRect.bottom;

        // 百分比 -> 客户区物理像素坐标
        int clickX = static_cast<int>(xPercent * clientW + 0.5);
        int clickY = static_cast<int>(yPercent * clientH + 0.5);

        // 转换为屏幕坐标（用于 SendInput）
        POINT screenPt = {clickX, clickY};
        ClientToScreen(hwnd, &screenPt);

        // 获取屏幕物理分辨率（必须在 DPI 感知模式内获取，与 screenPt 坐标系一致）
        int screenW = GetSystemMetrics(SM_CXSCREEN);
        int screenH = GetSystemMetrics(SM_CYSCREEN);

        // 恢复原 DPI 感知上下文
        if (pSetDpiCtx && oldDpiCtx) {
            pSetDpiCtx(oldDpiCtx);
        }

        if (screenW <= 0 || screenH <= 0) return false;

        // SendInput 使用 0~65535 归一化坐标
        LONG normX = static_cast<LONG>((screenPt.x * 65535LL + screenW / 2) / screenW);
        LONG normY = static_cast<LONG>((screenPt.y * 65535LL + screenH / 2) / screenH);

        INPUT inputs[3] = {};

        // 1. 移动鼠标到目标位置
        inputs[0].type       = INPUT_MOUSE;
        inputs[0].mi.dx      = normX;
        inputs[0].mi.dy      = normY;
        inputs[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;

        // 2. 鼠标左键按下
        inputs[1].type       = INPUT_MOUSE;
        inputs[1].mi.dx      = normX;
        inputs[1].mi.dy      = normY;
        inputs[1].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_LEFTDOWN;

        // 3. 鼠标左键松开
        inputs[2].type       = INPUT_MOUSE;
        inputs[2].mi.dx      = normX;
        inputs[2].mi.dy      = normY;
        inputs[2].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_LEFTUP;

        UINT sent = SendInput(3, inputs, sizeof(INPUT));
        return sent == 3;
#else
        (void)pid;
        (void)xPercent;
        (void)yPercent;
        return false;
#endif
    }

    bool triggerMinecraftUiReloadShortcut(int pid) {
#ifdef _WIN32
        static const std::wstring keyword = L"Minecraft";
        HWND hwnd = findWindowByPidAndTitleContains(static_cast<DWORD>(pid), keyword);
        if (!hwnd) {
            return false;
        }

        if (IsIconic(hwnd)) {
            return false;
        }

        auto makeKeyLParam = [](UINT vk, bool keyUp) -> LPARAM {
            UINT scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
            LPARAM lParam = 1 | (static_cast<LPARAM>(scan) << 16);
            if (keyUp) {
                lParam |= (1LL << 30) | (1LL << 31);
            }
            return lParam;
        };

        const bool ok =
            PostMessageW(hwnd, WM_KEYDOWN, VK_CONTROL, makeKeyLParam(VK_CONTROL, false)) &&
            PostMessageW(hwnd, WM_KEYDOWN, 'R', makeKeyLParam('R', false)) &&
            PostMessageW(hwnd, WM_KEYUP, 'R', makeKeyLParam('R', true)) &&
            PostMessageW(hwnd, WM_KEYUP, VK_CONTROL, makeKeyLParam(VK_CONTROL, true));
        return ok;
#else
        (void)pid;
        return false;
#endif
    }
} // namespace MCDevTool::Style
