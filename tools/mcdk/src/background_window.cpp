#include <mcdk/background_window.hpp>

#include <mcdk/console_output.hpp>

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

        constexpr auto  PollInterval   = std::chrono::milliseconds(50);
        constexpr DWORD UserInputGapMs = 400;

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
            const DWORD self    = GetCurrentThreadId();
            const DWORD holder  = GetWindowThreadProcessId(current, nullptr);
            const bool attached = holder != 0 && holder != self && AttachThreadInput(self, holder, TRUE);
            SetForegroundWindow(target);
            if (attached) {
                AttachThreadInput(self, holder, FALSE);
            }
        }

        bool userJustActed() {
            LASTINPUTINFO info{sizeof(info)};
            return GetLastInputInfo(&info) && GetTickCount() - info.dwTime < UserInputGapMs;
        }
#endif
    } // namespace

    BackgroundWindowGuard::~BackgroundWindowGuard() { stop(); }

    void BackgroundWindowGuard::start(std::uint32_t pid) {
        if (mThread.joinable() || pid == 0) {
            return;
        }
        mPid  = pid;
        mStop = false;
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

    void BackgroundWindowGuard::run() {
#ifdef _WIN32
        HWND                     lastOther = GetForegroundWindow();
        bool                     reported  = false;
        std::unordered_set<HWND> seen;
        NewWindows               windows{mPid, &seen};
        while (!mStop) {
            std::this_thread::sleep_for(PollInterval);
            EnumWindows(pushBackNew, reinterpret_cast<LPARAM>(&windows));

            HWND current = GetForegroundWindow();
            if (current == nullptr) {
                continue;
            }
            if (windowPid(current) != mPid) {
                lastOther = current;
                continue;
            }
            bool armed = false;
            {
                std::lock_guard lock(mMutex);
                armed = Clock::now() < mArmedUntil;
                if (armed && userJustActed()) {
                    // 用户自己切过来的，这一轮不再干预。
                    mArmedUntil = {};
                    armed       = false;
                }
            }
            if (!armed || lastOther == nullptr || !IsWindow(lastOther)) {
                continue;
            }
            pushBack(current);
            giveForegroundTo(lastOther, current);
            if (!reported) {
                reported = true;
                printColoredAtomic("[MCDK] 后台模式：游戏窗口抢占了前台，已交还", ConsoleColor::DarkGray);
            }
        }
#endif
    }

} // namespace mcdk
