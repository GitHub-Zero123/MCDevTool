// 游戏生命周期状态机（docs/plugin-system/05-interfaces.md §5.1）。
#include <mcdk/runtime/game_lifecycle.hpp>

#include <iostream>
#include <string>

namespace {

    bool expect(bool condition, const std::string& description) {
        if (!condition) {
            std::cerr << "Failed: " << description << '\n';
        }
        return condition;
    }

} // namespace

int main() {
    using mcdk::runtime::GameLifecycleState;
    using mcdk::runtime::GameLifecycleTracker;
    using mcdk::runtime::classifyGameLifecycle;

    bool passed = true;

    // --- 纯判定 ------------------------------------------------------
    // 关键的一对：当下都是零客户端，靠 everInWorld 区分。
    passed &= expect(
        classifyGameLifecycle(true, 0, false, false) == GameLifecycleState::Loading,
        "从未握过手 + 零客户端 = 还在加载"
    );
    passed &= expect(
        classifyGameLifecycle(true, 0, false, true) == GameLifecycleState::Menu,
        "握过手 + 零客户端 = 退回主菜单"
    );
    passed &= expect(classifyGameLifecycle(true, 1, false, true) == GameLifecycleState::InWorld, "有客户端 = 在世界里");
    passed &= expect(classifyGameLifecycle(true, 1, true, true) == GameLifecycleState::Exited, "退出优先于一切");
    passed &= expect(
        classifyGameLifecycle(false, 1, false, true) == GameLifecycleState::Unavailable,
        "调试能力未启用时无从判断"
    );

    // --- 追踪器：完整的一轮 ------------------------------------------
    {
        GameLifecycleTracker tracker;
        tracker.setDebugCapabilityEnabled(true);
        passed &= expect(tracker.state() == GameLifecycleState::Loading, "初始是加载中");

        const auto enter = tracker.onIpcClientCountChanged(1);
        passed &= expect(enter.has_value() && enter->to == GameLifecycleState::InWorld, "首次握手迁移到 InWorld");

        passed &= expect(!tracker.onIpcClientCountChanged(2).has_value(), "第二个客户端不改变状态");

        const auto leave = tracker.onIpcClientCountChanged(0);
        passed &= expect(
            leave.has_value() && leave->from == GameLifecycleState::InWorld && leave->to == GameLifecycleState::Menu,
            "断开后是主菜单而不是加载中——锁存位起作用了"
        );

        const auto exit = tracker.onMinecraftExited();
        passed &= expect(exit.has_value() && exit->to == GameLifecycleState::Exited, "退出迁移");
        passed &= expect(!tracker.onMinecraftExited().has_value(), "重复退出不再产生迁移");
    }

    // --- 调试能力未启用时不该有迁移 ------------------------------------
    {
        GameLifecycleTracker tracker;
        passed &= expect(tracker.state() == GameLifecycleState::Unavailable, "未启用调试能力");
        passed &= expect(!tracker.onIpcClientCountChanged(1).has_value(), "未启用时客户端变化不产生迁移");
    }

    std::cout << (passed ? "game_lifecycle_test passed\n" : "game_lifecycle_test failed\n");
    return passed ? 0 : 1;
}
