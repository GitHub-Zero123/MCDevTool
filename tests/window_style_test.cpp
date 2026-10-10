#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include "mcdevtool/style.h"

// fixed_size 配置的是客户区物理像素，非 100% 缩放下也必须一个像素不差。
// 在 100% 缩放的显示器上跑不出 DPI 相关的问题，需要在 125% / 150% / 200% 等缩放下运行。
namespace {
    using namespace MCDevTool::Style;

    void require(bool condition, const std::string& message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    // 跟 mcdk 一样走 MinecraftWindowStyler：样式在它自己的后台线程上应用，那个线程的 DPI 感知
    // 是进程默认值，而不是创建窗口的线程设置的值。
    class TestStyler : public MinecraftWindowStyler {
    public:
        using MinecraftWindowStyler::MinecraftWindowStyler;

        void onStyleApplied() override { applied = true; }

        std::atomic<bool> applied = false;
    };

    struct TestWindow {
        HWND hwnd = nullptr;

        void create() {
            WNDCLASSW cls{};
            cls.lpfnWndProc   = DefWindowProcW;
            cls.hInstance     = GetModuleHandleW(nullptr);
            cls.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
            cls.lpszClassName = L"MCDevToolStyleTest";
            require(RegisterClassW(&cls) != 0, "RegisterClass failed");
            hwnd = CreateWindowExW(
                0,
                cls.lpszClassName,
                L"Minecraft window style test",
                WS_OVERLAPPEDWINDOW,
                100,
                100,
                800,
                600,
                nullptr,
                nullptr,
                cls.hInstance,
                nullptr
            );
            require(hwnd != nullptr, "CreateWindow failed");
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        }

        // 恢复成带标题栏的普通窗口，让每个用例都从同一个起点开始。
        void reset() {
            SetWindowLongW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
            SetWindowPos(hwnd, nullptr, 100, 100, 800, 600, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            pump();
        }

        // 样式线程的 SetWindowPos 要靠窗口线程处理消息才能返回。
        void pump() {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }

        ~TestWindow() {
            if (hwnd) {
                DestroyWindow(hwnd);
            }
            UnregisterClassW(L"MCDevToolStyleTest", GetModuleHandleW(nullptr));
        }
    };

    void applyAndVerify(TestWindow& window, StyleConfig config, const std::string& label) {
        const WindowSize expected = config.fixedSize.value();
        window.reset();

        TestStyler styler(static_cast<int>(GetCurrentProcessId()), std::move(config));
        styler.start();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!styler.applied) {
            require(std::chrono::steady_clock::now() < deadline, label + ": style was not applied in time");
            window.pump();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        styler.join();
        window.pump();

        RECT client{};
        require(GetClientRect(window.hwnd, &client) != FALSE, "GetClientRect failed");
        const std::string actual = std::to_string(client.right) + "x" + std::to_string(client.bottom);
        require(
            client.right == expected.width && client.bottom == expected.height,
            label + ": client area is " + actual + ", expected " + std::to_string(expected.width) + "x"
                + std::to_string(expected.height)
        );
        std::cout << "PASS: " << label << " (" << actual << ")\n";
    }
} // namespace

int main() {
    // 窗口按每显示器 DPI 感知创建，跟游戏窗口一致；测试里读到的也都是物理像素。
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try {
        TestWindow window;
        window.create();
        window.pump();
        const UINT dpi = GetDpiForWindow(window.hwnd);
        std::cout << "Window DPI: " << dpi << " (" << dpi * 100 / 96 << "%)\n";
        if (dpi == USER_DEFAULT_SCREEN_DPI) {
            std::cout << "NOTE: running at 100% scaling, DPI-related regressions cannot show up here\n";
        }

        applyAndVerify(window, {.fixedSize = WindowSize{1280, 720}}, "fixed_size 1280x720");
        applyAndVerify(window, {.fixedSize = WindowSize{1001, 563}}, "fixed_size with odd numbers 1001x563");
        applyAndVerify(
            window,
            {.hideTitleBar = true, .fixedSize = WindowSize{1280, 720}},
            "fixed_size together with hide_title_bar"
        );
        applyAndVerify(
            window,
            {.alwaysOnTop = true, .fixedSize = WindowSize{854, 480}},
            "fixed_size together with always_on_top"
        );
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
