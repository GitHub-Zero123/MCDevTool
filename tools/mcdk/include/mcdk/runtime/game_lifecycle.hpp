#pragma once

//
// 游戏生命周期状态。判定规则见 docs/plugin-system/05-interfaces.md §5.1。
//

#include <atomic>
#include <cstddef>
#include <optional>
#include <string_view>

namespace mcdk::runtime {

    enum class GameLifecycleState {
        // 调试 IPC 未启用，无从判断。
        Unavailable,
        // 进程已创建，但从未握过手——还在加载。
        Loading,
        // 曾经握过手，现在没有客户端——退回主菜单了。
        Menu,
        // 调试 IPC 有客户端——在世界里。
        InWorld,
        // 游戏进程已退出。
        Exited,
    };

    // 给 MCP / Host Bridge 的 JSON 用。C++ 侧一律用枚举。
    [[nodiscard]] std::string_view gameLifecycleStateName(GameLifecycleState state) noexcept;

    // 纯判定。everInWorld 是「曾经握过手」的锁存位，它是 Loading 与 Menu 的唯一区分依据。
    [[nodiscard]] GameLifecycleState classifyGameLifecycle(
        bool        debugCapabilityEnabled,
        std::size_t ipcClientCount,
        bool        minecraftExited,
        bool        everInWorld
    ) noexcept;

    struct GameLifecycleTransition {
        GameLifecycleState from;
        GameLifecycleState to;
    };

    // 宿主内唯一的一份状态。Host Bridge 与插件宿主都读它——各自维护一份必然分叉。
    class GameLifecycleTracker {
    public:
        void setDebugCapabilityEnabled(bool enabled) noexcept;

        // 三个输入的更新点。返回值非空即表示状态真的变了，调用方据此发通知。
        [[nodiscard]] std::optional<GameLifecycleTransition> onIpcClientCountChanged(std::size_t count) noexcept;
        [[nodiscard]] std::optional<GameLifecycleTransition> onMinecraftExited() noexcept;

        [[nodiscard]] GameLifecycleState state() const noexcept;
        [[nodiscard]] bool               everInWorld() const noexcept;

    private:
        [[nodiscard]] GameLifecycleState compute() const noexcept;

        std::atomic<bool>        mDebugCapabilityEnabled{false};
        std::atomic<std::size_t> mIpcClientCount{0};
        std::atomic<bool>        mMinecraftExited{false};
        // 只会 false → true。
        std::atomic<bool> mEverInWorld{false};
    };

} // namespace mcdk::runtime
