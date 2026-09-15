// 针对真实窗口的输入注入回归测试：创建一个标题含 Minecraft 的窗口，
// 让引擎在工作线程注入输入，主线程泵消息并记录窗口实际收到的事件。
// 需要交互式桌面（会短暂抢占前台并移动鼠标），按需手动运行。
// 运行期间请勿操作鼠标键盘：真实输入会混进窗口收到的事件里，导致断言失真。
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "mcdevtool/window_input.h"

namespace {
    namespace Engine = MCDevTool::Input;

    using Clock = std::chrono::steady_clock;

    struct Event {
        UINT              message = 0;
        WPARAM            wParam  = 0;
        LPARAM            lParam  = 0;
        Clock::time_point at      = Clock::now();
    };

    std::vector<Event> events;

    void require(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    bool passed = true;

    bool expect(bool condition, const char* description) {
        if (!condition) {
            std::cerr << "Failed: " << description << '\n';
            passed = false;
        }
        return condition;
    }

    LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
        case WM_CHAR:
        case WM_MOUSEMOVE:
        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MOUSEWHEEL:
            events.push_back(Event{message, wParam, lParam, Clock::now()});
            break;
        default:
            break;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    struct TestWindow {
        HWND hwnd = nullptr;

        TestWindow() {
            WNDCLASSW cls{};
            cls.lpfnWndProc   = windowProc;
            cls.hInstance     = GetModuleHandleW(nullptr);
            cls.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
            cls.lpszClassName = L"MCDevToolInputTest";
            require(RegisterClassW(&cls) != 0, "RegisterClass failed");

            hwnd = CreateWindowExW(
                0,
                cls.lpszClassName,
                L"Minecraft input regression test",
                WS_OVERLAPPEDWINDOW,
                120,
                120,
                800,
                600,
                nullptr,
                nullptr,
                cls.hInstance,
                nullptr
            );
            require(hwnd != nullptr, "CreateWindow failed");

            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
            SetFocus(hwnd);
        }

        ~TestWindow() {
            if (hwnd != nullptr) DestroyWindow(hwnd);
        }

        [[nodiscard]] SIZE clientSize() const {
            RECT rect{};
            GetClientRect(hwnd, &rect);
            return SIZE{rect.right, rect.bottom};
        }
    };

    void pump() {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); // Unicode 输入要经过它才会变成 WM_CHAR
            DispatchMessageW(&message);
        }
    }

    // 引擎会阻塞式地按节奏投递输入，因此必须放到工作线程，主线程专心泵消息。
    Engine::Result<Engine::Report> runSteps(std::vector<Engine::Step> steps, Engine::Options options) {
        events.clear();

        const int pid    = static_cast<int>(GetCurrentProcessId());
        auto      worker = std::async(std::launch::async, [&] { return Engine::run(pid, steps, options); });

        while (worker.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            pump();
            Sleep(1);
        }
        for (int drain = 0; drain < 50; ++drain) {
            pump();
            Sleep(2);
        }
        return worker.get();
    }

    std::size_t countOf(UINT message) {
        std::size_t total = 0;
        for (const auto& event : events) {
            total += event.message == message ? 1 : 0;
        }
        return total;
    }

    const Event* firstOf(UINT message) {
        for (const auto& event : events) {
            if (event.message == message) return &event;
        }
        return nullptr;
    }

    std::uint16_t scanOf(const Event& event) { return static_cast<std::uint16_t>((event.lParam >> 16) & 0xFF); }

    int millisBetween(const Event& from, const Event& to) {
        return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(to.at - from.at).count());
    }

    Engine::Options baseOptions() {
        Engine::Options options;
        options.stepDelayMs = 30;
        return options;
    }

    void reportError(const char* label, const Engine::Error& error) {
        std::cerr << label << ": [" << Engine::errorCodeName(error.code) << "] " << error.message << '\n';
        passed = false;
    }
} // namespace

int main() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); // 与引擎同一坐标系

    TestWindow window;
    for (int settle = 0; settle < 60; ++settle) { // 等窗口打开动画结束，否则客户区尺寸还在变
        pump();
        Sleep(5);
    }

    const SIZE client = window.clientSize();
    require(client.cx > 0 && client.cy > 0, "test window has no client area");

    // --- 点击定位与长按时序 ---
    {
        auto options = baseOptions();
        auto report  = runSteps(
            {Engine::ClickStep{
                Engine::Point{0.5, 0.5},
                Engine::MouseButton::Left,
                Engine::PressAction::Press,
                200,
                 {}
            }},
            options
        );

        if (!report.has_value()) {
            reportError("click", report.error());
        } else {
            const auto* down = firstOf(WM_LBUTTONDOWN);
            const auto* up   = firstOf(WM_LBUTTONUP);
            expect(down != nullptr && up != nullptr, "a click produces both button down and button up");
            if (down != nullptr && up != nullptr) {
                const int  x        = static_cast<short>(LOWORD(down->lParam));
                const int  y        = static_cast<short>(HIWORD(down->lParam));
                const bool centered = std::abs(x - client.cx / 2) <= 5 && std::abs(y - client.cy / 2) <= 5;
                if (!centered) {
                    std::cerr << "  click landed at (" << x << ", " << y << ") in a " << client.cx << "x" << client.cy
                              << " client area\n";
                }
                expect(centered, "the click lands on the center of the client area");
                expect(millisBetween(*down, *up) >= 180, "hold_ms keeps the button down for the requested time");
            }
            expect(report->executed == 1 && report->released.empty(), "the batch leaves nothing held");
        }
    }

    // --- 扫描码：与键盘布局无关地验证按下的是哪个物理键 ---
    {
        auto report =
            runSteps({Engine::KeyStep{{*Engine::findKey("w")}, Engine::PressAction::Press, 50, 1}}, baseOptions());
        if (!report.has_value()) {
            reportError("key", report.error());
        } else {
            const auto* down = firstOf(WM_KEYDOWN);
            expect(down != nullptr && scanOf(*down) == 0x11, "the w key is sent as physical scan code 0x11");
        }
    }

    // --- 跨步骤长按：按下、等待、松开 ---
    {
        auto report = runSteps(
            {Engine::KeyStep{{*Engine::findKey("w")}, Engine::PressAction::Down, 0, 1},
             Engine::WaitStep{400},
             Engine::KeyStep{{*Engine::findKey("w")}, Engine::PressAction::Up, 0, 1}},
            baseOptions()
        );
        if (!report.has_value()) {
            reportError("hold", report.error());
        } else {
            const auto* down = firstOf(WM_KEYDOWN);
            const auto* up   = firstOf(WM_KEYUP);
            expect(down != nullptr && up != nullptr, "an explicit down/up pair reaches the window");
            if (down != nullptr && up != nullptr) {
                expect(millisBetween(*down, *up) >= 380, "the key stays down across the wait step");
            }
            expect(report->held.empty(), "a balanced hold leaves nothing registered as held");
        }
    }

    // --- 拖拽：按下与松开之间必须出现插值移动 ---
    {
        Engine::DragStep drag;
        drag.from     = Engine::Point{0.25, 0.5};
        drag.to       = Engine::Point{0.75, 0.5};
        drag.holdMs   = 60;
        drag.segments = 10;

        auto report = runSteps({drag}, baseOptions());
        if (!report.has_value()) {
            reportError("drag", report.error());
        } else {
            expect(firstOf(WM_LBUTTONDOWN) != nullptr, "the drag presses the button");

            // 定位到起点的移动发生在按下之前，按住期间的轨迹从第一个插值点开始。
            std::vector<int> allX;
            std::vector<int> pathX;
            int              down = 0;
            for (const auto& event : events) {
                down += event.message == WM_LBUTTONDOWN ? 1 : 0;
                down -= event.message == WM_LBUTTONUP ? 1 : 0;
                if (event.message == WM_MOUSEMOVE) {
                    allX.push_back(static_cast<short>(LOWORD(event.lParam)));
                    if (down > 0) pathX.push_back(allX.back());
                }
            }

            const int  startX = static_cast<int>(client.cx * 0.25);
            const int  endX   = static_cast<int>(client.cx * 0.75);
            const auto passes = [](const std::vector<int>& xs, int target) {
                return std::ranges::any_of(xs, [&](int x) { return std::abs(x - target) <= 5; });
            };

            expect(passes(allX, startX), "the drag positions at the start before pressing");
            expect(pathX.size() >= 8, "the drag emits interpolated move events while the button is held");
            expect(std::ranges::is_sorted(pathX), "the held path advances monotonically toward the target");

            const bool reached = passes(pathX, endX);
            if (!reached) {
                std::cerr << "  drag held path covered " << pathX.size() << " points, expected to reach " << endX
                          << "\n";
            }
            expect(reached, "the drag reaches the target while the button is held");
        }
    }

    // --- 滚轮与文本 ---
    {
        auto report = runSteps(
            {Engine::ScrollStep{Engine::Point{0.5, 0.5}, -2, Engine::ScrollAxis::Vertical, {}}, Engine::TextStep{"ab"}},
            baseOptions()
        );
        if (!report.has_value()) {
            reportError("scroll/text", report.error());
        } else {
            // 与真实滚轮一致：每格一个事件，总量等于请求的格数。
            int wheelTotal = 0;
            for (const auto& event : events) {
                if (event.message == WM_MOUSEWHEEL) wheelTotal += GET_WHEEL_DELTA_WPARAM(event.wParam);
            }
            expect(countOf(WM_MOUSEWHEEL) == 2 && wheelTotal == -240, "two notches arrive as two wheel events");

            std::string typed;
            for (const auto& event : events) {
                if (event.message == WM_CHAR) typed.push_back(static_cast<char>(event.wParam));
            }
            expect(typed == "ab", "text arrives as characters in order");
        }
    }

    // --- sync：锚定批次起点的绝对时刻，长按跨越 sync 不漂移 ---
    {
        auto options        = baseOptions();
        options.stepDelayMs = 0;

        auto report = runSteps(
            {Engine::KeyStep{{*Engine::findKey("w")}, Engine::PressAction::Down, 0, 1},
             Engine::SyncStep{300},
             Engine::KeyStep{{*Engine::findKey("w")}, Engine::PressAction::Up, 0, 1}},
            options
        );
        if (!report.has_value()) {
            reportError("sync", report.error());
        } else {
            const auto* down = firstOf(WM_KEYDOWN);
            const auto* up   = firstOf(WM_KEYUP);
            expect(down != nullptr && up != nullptr, "a sync-bounded hold reaches the window");
            if (down != nullptr && up != nullptr) {
                const int held = millisBetween(*down, *up);
                if (held < 280 || held > 450) {
                    std::cerr << "  sync released the key after " << held << " ms\n";
                }
                expect(held >= 280 && held <= 450, "sync releases the key at the scheduled absolute time");
            }
        }
    }

    // --- 坐标 1.0 是合法的右下角，落在最后一个像素上 ---
    {
        auto report = runSteps(
            {Engine::ClickStep{Engine::Point{1.0, 1.0}, Engine::MouseButton::Left, Engine::PressAction::Press, 20, {}}},
            baseOptions()
        );
        if (!report.has_value()) {
            reportError("corner", report.error());
        } else {
            expect(
                report->steps.size() == 1 && report->steps[0].at.has_value()
                    && report->steps[0].at->x == client.cx - 1 && report->steps[0].at->y == client.cy - 1,
                "(1.0, 1.0) resolves to the bottom-right pixel instead of being rejected"
            );
        }
    }

    // --- dry_run 不产生任何事件，但仍解析坐标 ---
    {
        auto options   = baseOptions();
        options.dryRun = true;

        auto report = runSteps(
            {Engine::ClickStep{Engine::Point{0.5, 0.5}, Engine::MouseButton::Left, Engine::PressAction::Press, 50, {}}},
            options
        );
        if (!report.has_value()) {
            reportError("dry_run", report.error());
        } else {
            expect(
                countOf(WM_LBUTTONDOWN) == 0 && countOf(WM_LBUTTONUP) == 0 && countOf(WM_KEYDOWN) == 0
                    && countOf(WM_CHAR) == 0 && countOf(WM_MOUSEWHEEL) == 0,
                "dry_run sends no input at all"
            );
            expect(
                report->steps.size() == 1 && report->steps[0].at.has_value() && report->steps[0].at->x == client.cx / 2,
                "dry_run still resolves the target pixel"
            );
        }
    }

    // --- leave_held 与 /release-all 的收尾 ---
    {
        auto options      = baseOptions();
        options.leaveHeld = true;

        auto report =
            runSteps({Engine::KeyStep{{*Engine::findKey("shift")}, Engine::PressAction::Down, 0, 1}}, options);
        if (!report.has_value()) {
            reportError("leave_held", report.error());
        } else {
            expect(report->held.size() == 1 && report->held[0] == "shift", "leave_held reports what stays down");
            expect(report->released.empty(), "leave_held does not report a held key as released");
            expect(countOf(WM_KEYUP) == 0, "leave_held does not release the key");
        }

        events.clear();
        auto released = Engine::releaseHeld(static_cast<int>(GetCurrentProcessId()));
        for (int drain = 0; drain < 50; ++drain) {
            pump();
            Sleep(2);
        }
        if (!released.has_value()) {
            reportError("release-all", released.error());
        } else {
            expect(released->released.size() == 1, "release-all reports the released input");
            expect(countOf(WM_KEYUP) == 1, "release-all actually sends the key up");
            expect(Engine::heldKeys().empty(), "nothing remains held afterwards");
        }
    }

    // --- 回归：拉弓这类跨调用鼠标长按也不能误报 released ---
    {
        auto options      = baseOptions();
        options.leaveHeld = true;

        auto report = runSteps(
            {Engine::ClickStep{
                Engine::Point{0.5, 0.5},
                Engine::MouseButton::Right,
                Engine::PressAction::Down,
                0,
                {}
            }},
            options
        );
        if (!report.has_value()) {
            reportError("leave_held(mouse)", report.error());
        } else {
            expect(report->held == std::vector<std::string>{"mouse:right"}, "the right button remains held");
            expect(report->released.empty(), "a held mouse button is not reported as released");
            expect(countOf(WM_RBUTTONDOWN) == 1 && countOf(WM_RBUTTONUP) == 0, "the mouse stays down across calls");
        }

        auto released = runSteps(
            {Engine::ClickStep{std::nullopt, Engine::MouseButton::Right, Engine::PressAction::Up, 0, {}}},
            baseOptions()
        );
        if (!released.has_value()) {
            reportError("explicit up(mouse)", released.error());
        } else {
            expect(released->held.empty(), "the right button is no longer held after release");
            expect(countOf(WM_RBUTTONUP) == 1, "the explicit mouse release reaches the window");
        }
    }

    // --- 回归：显式 up 必须同时销掉跨调用登记表，否则 held 只增不减 ---
    {
        auto options      = baseOptions();
        options.leaveHeld = true;

        auto held = runSteps({Engine::KeyStep{{*Engine::findKey("shift")}, Engine::PressAction::Down, 0, 1}}, options);
        if (!held.has_value()) {
            reportError("leave_held(again)", held.error());
        } else {
            expect(Engine::heldKeys().size() == 1, "the key is registered as held across calls");
        }

        // 再按一次同一个键：登记表里不应出现两条。
        auto again = runSteps({Engine::KeyStep{{*Engine::findKey("shift")}, Engine::PressAction::Down, 0, 1}}, options);
        if (!again.has_value()) {
            reportError("leave_held(duplicate)", again.error());
        } else {
            expect(Engine::heldKeys().size() == 1, "re-pressing a held key does not duplicate the registry entry");
        }

        auto released =
            runSteps({Engine::KeyStep{{*Engine::findKey("shift")}, Engine::PressAction::Up, 0, 1}}, baseOptions());
        if (!released.has_value()) {
            reportError("explicit up", released.error());
        } else {
            expect(Engine::heldKeys().empty(), "an explicit up clears the cross-call registry");
            expect(released->held.empty(), "the report no longer lists the released key as held");
        }
    }

    // --- 失焦守卫：焦点不在目标窗口时 require 策略必须拒绝而不是乱发 ---
    {
        auto options  = baseOptions();
        options.focus = Engine::FocusPolicy::Require;

        ShowWindow(window.hwnd, SW_MINIMIZE);
        pump();
        Sleep(120);

        options.restoreIfMinimized = false;
        auto report                = runSteps(
            {Engine::ClickStep{Engine::Point{0.5, 0.5}, Engine::MouseButton::Left, Engine::PressAction::Press, 10, {}}},
            options
        );
        expect(!report.has_value(), "a minimized window is refused instead of being clicked blindly");
        expect(countOf(WM_LBUTTONDOWN) == 0 && countOf(WM_KEYDOWN) == 0, "no input is sent when preflight refuses");

        ShowWindow(window.hwnd, SW_RESTORE);
        pump();
    }

    std::cout << (passed ? "window input tests passed" : "window input tests failed") << '\n';
    return passed ? 0 : 1;
}
