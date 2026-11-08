#include <mcdevtool/window_input.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// 在本进程里建一个标题含 Minecraft 的窗口，逐条核对 Delivery::Post 投递出的消息。
namespace {
    namespace Input = MCDevTool::Input;

    struct Message {
        UINT   id;
        WPARAM wParam;
        LPARAM lParam;
    };

    std::mutex           gMutex;
    std::vector<Message> gMessages;
    std::atomic<HWND>    gWindow{nullptr};

    void require(bool condition, const std::string& message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    bool isInputMessage(UINT id) {
        return (id >= WM_KEYFIRST && id <= WM_KEYLAST) || (id >= WM_MOUSEFIRST && id <= WM_MOUSELAST);
    }

    LRESULT CALLBACK windowProc(HWND window, UINT id, WPARAM wParam, LPARAM lParam) {
        if (isInputMessage(id)) {
            std::lock_guard lock(gMutex);
            gMessages.push_back({id, wParam, lParam});
            return 0;
        }
        return DefWindowProcW(window, id, wParam, lParam);
    }

    void pumpWindow() {
        WNDCLASSW windowClass{};
        windowClass.lpfnWndProc   = windowProc;
        windowClass.hInstance     = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = L"McdkPostInputTest";
        RegisterClassW(&windowClass);

        HWND window = CreateWindowExW(
            WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            windowClass.lpszClassName,
            L"Minecraft post input test",
            WS_POPUP,
            -3000,
            -3000,
            400,
            200,
            nullptr,
            nullptr,
            windowClass.hInstance,
            nullptr
        );
        ShowWindow(window, SW_SHOWNOACTIVATE);
        gWindow = window;

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            DispatchMessageW(&message);
        }
        DestroyWindow(window);
    }

    std::vector<Message> takeMessages() {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        std::lock_guard lock(gMutex);
        return std::exchange(gMessages, {});
    }

    Input::Report runSteps(std::vector<Input::Step> steps, Input::Options options = {}) {
        options.delivery    = Input::Delivery::Post;
        options.stepDelayMs = 0;
        auto report         = Input::run(static_cast<int>(GetCurrentProcessId()), steps, options);
        require(report.has_value(), report ? "" : report.error().message);
        return *report;
    }

    Input::Key key(std::string_view name) {
        const auto found = Input::findKey(name);
        require(found.has_value(), "unknown key");
        return *found;
    }

    void testKeyPress() {
        const auto w = key("w");
        (void)runSteps({Input::KeyStep{{w}, Input::PressAction::Press, 0, 1}});
        const auto messages = takeMessages();
        require(messages.size() == 2, "key press posts down and up");
        require(messages[0].id == WM_KEYDOWN && messages[0].wParam == 'W', "key down uses the virtual key");
        require(((messages[0].lParam >> 16) & 0xFF) == w.scan, "key down carries the scan code");
        require(messages[1].id == WM_KEYUP, "key up follows");
        require((static_cast<UINT>(messages[1].lParam) >> 30) == 3, "key up sets the transition bits");
    }

    void testSideSpecificKeysAreGeneric() {
        (void)runSteps({Input::KeyStep{{key("shift")}, Input::PressAction::Press, 0, 1}});
        const auto messages = takeMessages();
        require(messages.size() == 2 && messages[0].wParam == VK_SHIFT, "shift reports the generic virtual key");
    }

    void testClickUsesClientCoordinates() {
        (void)runSteps({Input::ClickStep{Input::Point{0.5, 0.25}, Input::MouseButton::Left, Input::PressAction::Press, 0, {}}});
        const auto messages = takeMessages();
        require(messages.size() == 3, "click posts move, down and up");
        require(messages[0].id == WM_MOUSEMOVE, "pointer moves first");
        require(LOWORD(messages[0].lParam) == 200 && HIWORD(messages[0].lParam) == 50, "client coordinates");
        require(messages[1].id == WM_LBUTTONDOWN && messages[1].wParam == MK_LBUTTON, "button state is reported");
        require(messages[2].id == WM_LBUTTONUP && messages[2].wParam == 0, "button state clears");
        require(messages[2].lParam == messages[0].lParam, "button events stay at the pointer");
    }

    void testClickInPlaceReusesThePointer() {
        (void)runSteps({Input::ClickStep{std::nullopt, Input::MouseButton::Right, Input::PressAction::Press, 0, {}}});
        const auto messages = takeMessages();
        require(messages.size() == 2 && messages[0].id == WM_RBUTTONDOWN, "in-place click does not move");
        require(LOWORD(messages[0].lParam) == 200 && HIWORD(messages[0].lParam) == 50, "last pointer is reused");
    }

    void testDragHoldsTheButtonWhileMoving() {
        Input::DragStep drag;
        drag.from     = {0.1, 0.1};
        drag.to       = {0.9, 0.9};
        drag.holdMs   = 0;
        drag.segments = 4;
        (void)runSteps({drag});
        const auto messages = takeMessages();
        require(messages.size() == 7, "drag posts start, down, four segments and up");
        require(messages[1].id == WM_LBUTTONDOWN, "button goes down at the start");
        for (std::size_t index = 2; index < 6; ++index) {
            require(messages[index].id == WM_MOUSEMOVE, "segments are moves");
            require(messages[index].wParam == MK_LBUTTON, "moves report the held button");
        }
    }

    void testWheelUsesScreenCoordinates() {
        (void)runSteps({Input::ScrollStep{Input::Point{0.5, 0.5}, -2, Input::ScrollAxis::Vertical, {}}});
        const auto messages = takeMessages();
        require(messages.size() == 3 && messages[1].id == WM_MOUSEWHEEL, "one wheel message per notch");
        require(GET_WHEEL_DELTA_WPARAM(messages[2].wParam) == -WHEEL_DELTA, "wheel delta");

        POINT expected{200, 100};
        ClientToScreen(gWindow.load(), &expected);
        require(
            static_cast<short>(LOWORD(messages[1].lParam)) == expected.x
                && static_cast<short>(HIWORD(messages[1].lParam)) == expected.y,
            "wheel coordinates are on screen"
        );
    }

    void testTextPostsCharacters() {
        (void)runSteps({Input::TextStep{"a测"}});
        const auto messages = takeMessages();
        require(messages.size() == 2, "one message per character");
        require(messages[0].id == WM_CHAR && messages[0].wParam == L'a', "ascii character");
        require(messages[1].id == WM_CHAR && messages[1].wParam == L'测', "chinese character");
    }

    void testModifierStateReachesMouseMessages() {
        Input::ClickStep click{Input::Point{0.5, 0.5}, Input::MouseButton::Left, Input::PressAction::Press, 0, {key("shift")}};
        (void)runSteps({click});
        const auto messages = takeMessages();
        const auto down     = std::ranges::find(messages, UINT{WM_LBUTTONDOWN}, &Message::id);
        require(down != messages.end() && (down->wParam & MK_SHIFT) != 0, "shift is reported on the click");
    }

    void testHeldInputIsReleasedThroughTheWindow() {
        Input::Options options;
        options.leaveHeld = true;
        (void)runSteps({Input::KeyStep{{key("w")}, Input::PressAction::Down, 0, 1}}, options);
        require(takeMessages().size() == 1, "only the key down is posted");
        require(Input::heldKeys().size() == 1, "the key stays registered");

        const auto released = Input::releaseHeld(static_cast<int>(GetCurrentProcessId()));
        require(released.has_value() && released->released.size() == 1, "release reports the key");
        const auto messages = takeMessages();
        require(messages.size() == 1 && messages[0].id == WM_KEYUP, "release goes back to the window");
    }

    void testFailedBatchReleasesKeys() {
        Input::Options options;
        options.budgetMs = 1;
        const std::vector<Input::Step> steps{
            Input::KeyStep{{key("w")}, Input::PressAction::Down, 0, 1},
            Input::WaitStep{200},
        };
        options.delivery = Input::Delivery::Post;
        const auto result = Input::run(static_cast<int>(GetCurrentProcessId()), steps, options);
        require(!result.has_value(), "the batch runs out of budget");
        const auto messages = takeMessages();
        require(messages.empty() || messages.back().id == WM_KEYUP, "no key stays down after a failure");
    }

    void testLookIsRejected() {
        Input::Options options;
        options.delivery = Input::Delivery::Post;
        const std::vector<Input::Step> steps{Input::LookStep{10.0, 0.0, 1}};
        const auto result = Input::run(0, steps, options);
        require(!result.has_value() && result.error().code == Input::ErrorCode::Unsupported, "look is unsupported");
        require(result.error().message.find("turn") != std::string::npos, "the error names the alternative");
    }

    void testDryRunPostsNothing() {
        Input::Options options;
        options.dryRun = true;
        (void)runSteps({Input::ClickStep{Input::Point{0.5, 0.5}, Input::MouseButton::Left, Input::PressAction::Press, 0, {}}}, options);
        require(takeMessages().empty(), "dry run posts nothing");
    }
} // namespace

int main() {
    std::thread pump(pumpWindow);
    const auto  stopPump = [&pump] {
        const auto id = GetThreadId(pump.native_handle());
        PostThreadMessageW(id, WM_QUIT, 0, 0);
        pump.join();
    };
    while (gWindow.load() == nullptr) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const HWND foreground = GetForegroundWindow();

    try {
        testKeyPress();
        testSideSpecificKeysAreGeneric();
        testClickUsesClientCoordinates();
        testClickInPlaceReusesThePointer();
        testDragHoldsTheButtonWhileMoving();
        testWheelUsesScreenCoordinates();
        testTextPostsCharacters();
        testModifierStateReachesMouseMessages();
        testHeldInputIsReleasedThroughTheWindow();
        testFailedBatchReleasesKeys();
        testLookIsRejected();
        testDryRunPostsNothing();

        require(GetForegroundWindow() != gWindow.load(), "the test window never takes the foreground");
        require(foreground == nullptr || IsWindow(foreground), "the foreground window is untouched");
    } catch (const std::exception& exception) {
        std::cerr << "FAILED: " << exception.what() << '\n';
        stopPump();
        return EXIT_FAILURE;
    }
    stopPump();
    std::cout << "window post input tests passed\n";
    return EXIT_SUCCESS;
}
