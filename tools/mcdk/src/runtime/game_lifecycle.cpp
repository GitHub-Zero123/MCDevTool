#include <mcdk/runtime/game_lifecycle.hpp>

namespace mcdk::runtime {

    std::string_view gameLifecycleStateName(GameLifecycleState state) noexcept {
        // 取值与 Host Bridge 协议里既有的字符串逐字一致，不另造一套。
        switch (state) {
        case GameLifecycleState::Loading:
            return "process_started";
        case GameLifecycleState::Menu:
            return "game_unavailable";
        case GameLifecycleState::InWorld:
            return "game_ready";
        case GameLifecycleState::Exited:
            return "exited";
        case GameLifecycleState::Unavailable:
        default:
            return "game_unavailable";
        }
    }

    GameLifecycleState classifyGameLifecycle(
        bool        debugCapabilityEnabled,
        std::size_t ipcClientCount,
        bool        minecraftExited,
        bool        everInWorld
    ) noexcept {
        if (minecraftExited) {
            return GameLifecycleState::Exited;
        }
        if (!debugCapabilityEnabled) {
            return GameLifecycleState::Unavailable;
        }
        if (ipcClientCount > 0) {
            return GameLifecycleState::InWorld;
        }
        return everInWorld ? GameLifecycleState::Menu : GameLifecycleState::Loading;
    }

    void GameLifecycleTracker::setDebugCapabilityEnabled(bool enabled) noexcept {
        mDebugCapabilityEnabled.store(enabled, std::memory_order_relaxed);
    }

    GameLifecycleState GameLifecycleTracker::compute() const noexcept {
        return classifyGameLifecycle(
            mDebugCapabilityEnabled.load(std::memory_order_relaxed),
            mIpcClientCount.load(std::memory_order_relaxed),
            mMinecraftExited.load(std::memory_order_relaxed),
            mEverInWorld.load(std::memory_order_relaxed)
        );
    }

    std::optional<GameLifecycleTransition> GameLifecycleTracker::onIpcClientCountChanged(std::size_t count) noexcept {
        const auto before = compute();
        mIpcClientCount.store(count, std::memory_order_relaxed);
        if (count > 0) {
            mEverInWorld.store(true, std::memory_order_relaxed);
        }
        const auto after = compute();
        if (before == after) {
            return std::nullopt;
        }
        return GameLifecycleTransition{before, after};
    }

    std::optional<GameLifecycleTransition> GameLifecycleTracker::onMinecraftExited() noexcept {
        const auto before = compute();
        mMinecraftExited.store(true, std::memory_order_relaxed);
        const auto after = compute();
        if (before == after) {
            return std::nullopt;
        }
        return GameLifecycleTransition{before, after};
    }

    GameLifecycleState GameLifecycleTracker::state() const noexcept { return compute(); }

    bool GameLifecycleTracker::everInWorld() const noexcept { return mEverInWorld.load(std::memory_order_relaxed); }

} // namespace mcdk::runtime
