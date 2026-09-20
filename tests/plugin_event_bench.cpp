// 零插件开销基准（docs/plugin-system/12-performance.md §6）。
// 无订阅者时每个发射点的额外成本上限为 15ns，容忍 CI 噪声。
#include <mcdk/plugin_host/events.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {
// 无订阅者时每个发射点的额外成本上限为 15ns，容忍 CI 噪声。
    constexpr double kMaxNanosPerEmitWithNoSubscribers = 15.0;

    constexpr int kIterations = 2'000'000;
    constexpr int kRepeats    = 5;

    volatile std::size_t gSink = 0;

    // 一行典型的 Minecraft 日志。长度会影响 C 组（要深拷贝），不影响 A/B。
    const std::string kLine =
        "[2026-09-21 10:32:11][INFO][Developer] ModMain.py:142 player entered dimension 0 at (128, 64, -512)";

    [[nodiscard]] double measureBaseline() {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kIterations; ++i) {
            gSink = gSink + kLine.size();
        }
        const auto end = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::nano>(end - start).count() / kIterations;
    }

    [[nodiscard]] double measureWithEmit(int iterations = kIterations) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < iterations; ++i) {
            // 与 game_process/logging.cpp 里的真实发射点保持同一形状：payload
            // 工厂是个 lambda，时间戳与取址都只在确有订阅者时才执行。
            const bool muted = MCDK_EMIT_VETOABLE(mcdk::plugin_host::EventId::LogLine, [&] {
                mcdk_ev_log_line payload{};
                payload.struct_size  = static_cast<std::uint32_t>(sizeof(payload));
                payload.channel      = 0;
                payload.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::system_clock::now().time_since_epoch()
                )
                                           .count();
                payload.text.ptr = kLine.data();
                payload.text.len = kLine.size();
                return payload;
            });
            if (!muted) {
                gSink = gSink + kLine.size();
            }
        }
        const auto end = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::nano>(end - start).count() / iterations;
    }

    // 取多轮的最小值：最小值代表「没有被调度器、中断、降频打扰的那一轮」，
    // 比平均值稳定得多，也正是我们想量的东西。
    template <class Measure>
    [[nodiscard]] double best(Measure&& measure) {
        double result = measure();
        for (int i = 1; i < kRepeats; ++i) {
            result = std::min(result, measure());
        }
        return result;
    }

    mcdk_event_result MCDK_CALL noopHandler(const mcdk_event*, void*) { return MCDK_EVENT_CONTINUE; }

} // namespace

int main() {
    using namespace mcdk::plugin_host;

#if MCDK_ENABLE_PLUGINS
    std::cout << "group: B/C (MCDK_ENABLE_PLUGINS=1)\n";
#else
    std::cout << "group: A (MCDK_ENABLE_PLUGINS=0)\n";
#endif

    const double baseline = best([] { return measureBaseline(); });
    const double noSubs   = best([] { return measureWithEmit(); });
    const double costB    = noSubs - baseline;

    std::cout << "baseline            " << baseline << " ns/iter\n";
    std::cout << "emit, 0 subscribers " << noSubs << " ns/iter\n";
    std::cout << "=> cost per emit    " << costB << " ns\n";

    bool passed = true;
#if MCDK_ENABLE_PLUGINS
    if (costB > kMaxNanosPerEmitWithNoSubscribers) {
        std::cerr << "Failed: 零插件开销契约被打破——每次发射多付了 " << costB << " ns，上限 "
                  << kMaxNanosPerEmitWithNoSubscribers << " ns\n"
                  << "        多半是某个 payload 构造跑到了订阅计数判断的外面，"
                     "见 docs/plugin-system/12-performance.md §1。\n";
        passed = false;
    }

    // --- C 组：一个 QUEUED 订阅者 ------------------------------------
    // 只记录不判定。它的成本里包含深拷贝与入队，本来就该比 B 高一两个数量级。
    const auto token = subscribeEvent(/*owner=*/1, EventId::LogLine, MCDK_DISPATCH_QUEUED, 0, &noopHandler, nullptr);
    if (token == 0) {
        std::cerr << "Failed: 无法订阅 mcdk.log.line\n";
        passed = false;
    } else {
        // 迭代次数必须明显小于队列上限（4096）。跑满了的话量到的就不再是
        // 「一次深拷贝 + 入队」，而是队满丢弃路径上与派发线程的锁争用。
        constexpr int kSubscribedIterations = 2000;
        const double  withSub               = measureWithEmit(kSubscribedIterations);
        std::cout << "emit, 1 subscriber  " << withSub << " ns/iter (记录用，不设阈值)\n";
        unsubscribeEvent(/*owner=*/1, token);
    }
    shutdownEventBus();
#else
    // A 组：发射点已被预处理掉，两条循环应当编译成同一份代码。
    std::cout << "（A 组：发射点已在预处理阶段消失，上面的差值即测量噪声本身）\n";
#endif

    std::cout << (passed ? "plugin_event_bench passed\n" : "plugin_event_bench failed\n");
    return passed ? 0 : 1;
}
