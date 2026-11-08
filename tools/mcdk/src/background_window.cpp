#include <mcdk/background_window.hpp>

#include <mcdk/console_output.hpp>

#include <optional>
#include <string_view>
#include <unordered_set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace mcdk {
    namespace {
        using Clock = std::chrono::steady_clock;

        constexpr DWORD ScanIntervalMs = 50;
        constexpr DWORD UserInputGapMs = 400;
        // 游戏在主窗口出现前后会自己抢前台；这段时间里不管用户有没有输入都交还。
        constexpr auto LaunchPhase = std::chrono::seconds(5);

#ifdef _WIN32
        DWORD windowPid(HWND window) {
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            return pid;
        }

        void pushBack(HWND window) {
            SetWindowPos(window, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }

        struct NewWindows {
            DWORD                     pid = 0;
            std::unordered_set<HWND>* seen = nullptr;
        };

        // 游戏新出现的可见顶层窗口各压到底一次。SW_SHOWNOACTIVATE 不激活，但仍会盖在别的窗口上。
        BOOL CALLBACK pushBackNew(HWND window, LPARAM context) {
            auto& windows = *reinterpret_cast<NewWindows*>(context);
            if (windowPid(window) == windows.pid && IsWindowVisible(window) && windows.seen->insert(window).second) {
                pushBack(window);
            }
            return TRUE;
        }

        // 前台切换的权限看的是调用线程，借用当前前台线程的输入状态才能把前台交回去。
        void giveForegroundTo(HWND target, HWND current) {
            const DWORD self     = GetCurrentThreadId();
            const DWORD holder   = GetWindowThreadProcessId(current, nullptr);
            const bool  attached = holder != 0 && holder != self && AttachThreadInput(self, holder, TRUE);
            SetForegroundWindow(target);
            if (attached) {
                AttachThreadInput(self, holder, FALSE);
            }
        }

        bool userJustActed() {
            LASTINPUTINFO info{sizeof(info)};
            return GetLastInputInfo(&info) && GetTickCount() - info.dwTime < UserInputGapMs;
        }

        bool isTaskbar(HWND window) {
            wchar_t name[64]{};
            GetClassNameW(window, name, static_cast<int>(std::size(name)));
            return std::wstring_view(name) == L"Shell_TrayWnd" || std::wstring_view(name) == L"Shell_SecondaryTrayWnd";
        }

        // 只有 Alt 切换、或刚点在游戏窗口/任务栏上，才算用户自己切过来；在别的窗口里打字不算。
        bool userSwitchedTo(HWND game) {
            if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) {
                return true;
            }
            if (!userJustActed()) {
                return false;
            }
            POINT cursor{};
            if (!GetCursorPos(&cursor)) {
                return false;
            }
            const HWND under = GetAncestor(WindowFromPoint(cursor), GA_ROOT);
            return under == game || (under != nullptr && isTaskbar(under));
        }

        struct GuardState {
            BackgroundWindowGuard*           guard = nullptr;
            DWORD                            pid   = 0;
            HWND                             lastOther = nullptr;
            bool                             reported  = false;
            std::unordered_set<HWND>         seen;
            std::optional<Clock::time_point> firstShown;
        };

        // WinEvent 回调没有上下文参数；每个 mcdk 进程只有一个看守，用线程局部变量传递。
        thread_local GuardState* gState = nullptr;
#endif
    } // namespace

    BackgroundWindowGuard::~BackgroundWindowGuard() { stop(); }

    void BackgroundWindowGuard::start(std::uint32_t pid) {
        if (mThread.joinable() || pid == 0) {
            return;
        }
        mPid    = pid;
        mStop   = false;
        mThread = std::thread([this] { run(); });
    }

    void BackgroundWindowGuard::armFor(std::chrono::milliseconds duration) {
        std::lock_guard lock(mMutex);
        mArmedUntil = std::max(mArmedUntil, Clock::now() + duration);
    }

    void BackgroundWindowGuard::stop() {
        mStop = true;
        if (mThread.joinable()) {
            mThread.join();
        }
    }

    bool BackgroundWindowGuard::shouldReturn(void* game, bool launching) {
#ifdef _WIN32
        std::lock_guard lock(mMutex);
        if (launching) {
            return true;
        }
        if (Clock::now() >= mArmedUntil) {
            return false;
        }
        if (userSwitchedTo(static_cast<HWND>(game))) {
            mArmedUntil = {}; // 用户自己切过来的，这一轮不再干预
            return false;
        }
        return true;
#else
        (void)game;
        (void)launching;
        return false;
#endif
    }

#ifdef _WIN32
    namespace {
        void handleForeground(HWND current) {
            auto& s = *gState;
            if (current == nullptr) {
                return;
            }
            if (windowPid(current) != s.pid) {
                s.lastOther = current;
                return;
            }
            const bool launching = !s.firstShown || Clock::now() - *s.firstShown < LaunchPhase;
            if (s.lastOther == nullptr || !IsWindow(s.lastOther) || !s.guard->shouldReturn(current, launching)) {
                return;
            }
            pushBack(current);
            giveForegroundTo(s.lastOther, current);
            if (!s.reported) {
                s.reported = true;
                printColoredAtomic("[MCDK] 后台模式：游戏窗口抢占了前台，已交还", ConsoleColor::DarkGray);
            }
        }

        void CALLBACK onForeground(HWINEVENTHOOK, DWORD, HWND window, LONG object, LONG, DWORD, DWORD) {
            if (object == OBJID_WINDOW && gState != nullptr) {
                handleForeground(window);
            }
        }
    } // namespace
#endif

    void BackgroundWindowGuard::run() {
#ifdef _WIN32
        GuardState state;
        state.guard     = this;
        state.pid       = mPid;
        state.lastOther = GetForegroundWindow();
        gState          = &state;
        NewWindows windows{mPid, &state.seen};

        // 前台一变就在回调里处理，不等轮询，缩短游戏占着前台的时间。
        const HWINEVENTHOOK hook =
            SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, &onForeground, 0, 0, WINEVENT_OUTOFCONTEXT);
        while (!mStop) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, ScanIntervalMs, QS_ALLINPUT);
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            EnumWindows(pushBackNew, reinterpret_cast<LPARAM>(&windows));
            if (!state.seen.empty() && !state.firstShown) {
                state.firstShown = Clock::now();
            }
            // 兜底：钩子没装上或漏了事件时，按轮询结果处理。
            handleForeground(GetForegroundWindow());
        }
        if (hook != nullptr) {
            UnhookWinEvent(hook);
        }
        gState = nullptr;
#endif
    }

} // namespace mcdk
