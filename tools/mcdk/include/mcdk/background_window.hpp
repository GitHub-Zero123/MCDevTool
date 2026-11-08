#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>

namespace mcdk {

    // 后台模式：让游戏窗口待在用户窗口之下，并在游戏自己抢走前台时还给用户。
    // 只在启动和进出存档后的一小段时间里看守；用户刚有输入时视为用户主动切换，不干预。
    class BackgroundWindowGuard {
    public:
        BackgroundWindowGuard() = default;
        ~BackgroundWindowGuard();

        BackgroundWindowGuard(const BackgroundWindowGuard&)            = delete;
        BackgroundWindowGuard& operator=(const BackgroundWindowGuard&) = delete;

        void start(std::uint32_t pid);
        // 在接下来的 duration 内看守前台。
        void armFor(std::chrono::milliseconds duration);
        void stop();

    private:
        void run();

        std::uint32_t                         mPid = 0;
        std::thread                           mThread;
        std::atomic<bool>                     mStop = false;
        std::mutex                            mMutex;
        std::chrono::steady_clock::time_point mArmedUntil{};
    };

} // namespace mcdk
