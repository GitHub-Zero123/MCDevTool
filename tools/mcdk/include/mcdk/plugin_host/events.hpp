#pragma once

//
// 事件总线的热路径部分。
//
// 零插件开销契约（docs/plugin-system/12-performance.md §1）：未加载任何插件时，
// 每个发射点的开销必须不超过「一次 relaxed 原子读 + 一次可预测分支」。
//
// 为此，发射点一律走下面的宏，payload 用工厂 lambda 惰性构造 —— 时间戳、字符串
// 转换、路径转 UTF-8 全部只能发生在确认有订阅者之后。写成
// `emit(EventId::LogLine, buildPayload(line))` 在语法上更自然，但那样零插件时
// 仍要付全部构造成本，是本文件存在的全部意义所在。
//

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <mcdk/plugin/abi/events.h>

namespace mcdk::plugin_host {

    // 扁平的内部索引，与 docs/plugin-system/13-registry.md §4.1 的事件登记表一一对应。
    // 数值可以随意重排：对外稳定的是名字，ABI 侧的 id 由 resolve() 在运行期映射。
    enum class EventId : std::uint32_t {
        McpRegisterBefore = 0,
        McpRegisterFinish,
        GameLaunchBefore,
        GameLaunchFinish,
        GameExit,
        LogLine,
        LogError,
        IpcClientConnected,
        IpcClientDisconnected,
        Count
    };

    inline constexpr std::size_t kEventCount = static_cast<std::size_t>(EventId::Count);

    // 唯一的热路径状态：9 个 uint32，一条 cache line 装得下，常驻 L1。
    // 只由 subscribe / unsubscribe 维护，禁止在别处直接改。
    extern std::array<std::atomic<std::uint32_t>, kEventCount> gSubscriberCount;

    [[nodiscard]] inline bool hasSubscribers(EventId id) noexcept {
        return gSubscriberCount[static_cast<std::size_t>(id)].load(std::memory_order_relaxed) != 0;
    }

    // 下面两个只在确有订阅者时才会被调用，因此可以随便做重活。
    void               dispatchRaw(EventId id, const void* payload, std::uint32_t payloadSize);
    [[nodiscard]] bool dispatchRawVetoable(EventId id, const void* payload, std::uint32_t payloadSize);

    template <class Factory>
    void dispatch(EventId id, Factory&& makePayload) {
        auto payload = makePayload();
        dispatchRaw(id, &payload, static_cast<std::uint32_t>(sizeof(payload)));
    }

    // 返回 true 表示被某个 SYNC 订阅者否决。QUEUED 订阅者无法否决——它们是
    // 异步的，等不到结果。
    template <class Factory>
    [[nodiscard]] bool dispatchVetoable(EventId id, Factory&& makePayload) {
        auto payload = makePayload();
        return dispatchRawVetoable(id, &payload, static_cast<std::uint32_t>(sizeof(payload)));
    }

    // 事件名 ↔ 内部 id。未知名字返回 kInvalidAbiEventId(0)。
    inline constexpr std::uint32_t kInvalidAbiEventId = 0;
    [[nodiscard]] std::uint32_t    toAbiEventId(EventId id) noexcept;
    [[nodiscard]] bool             fromAbiEventId(std::uint32_t abiId, EventId& out) noexcept;
    [[nodiscard]] std::uint32_t    resolveEventName(std::string_view name) noexcept;
    [[nodiscard]] std::string_view eventName(EventId id) noexcept;

    // 订阅管理。由 mcdk.events 的 shim 调用，owner 是插件句柄。
    [[nodiscard]] mcdk_handle subscribeEvent(
        mcdk_handle        owner,
        EventId            id,
        mcdk_dispatch_mode mode,
        std::int32_t       priority,
        mcdk_event_handler handler,
        void*              user
    );
    void unsubscribeEvent(mcdk_handle owner, mcdk_handle token);

    void postMainThreadWork(void(MCDK_CALL* fn)(void*), void* user);

    // 抽干投递到主线程的工作。宿主在阶段推进点与游戏等待循环中调用。
    // 无待办时是一次原子读 + 分支。
    void pumpMainThreadWork();

    // 创建主线程唤醒信号。只在确实加载了插件时调用一次。
    //
    // 零插件时不创建是刻意的：没有信号句柄，等待循环就退回无期限阻塞，
    // 主线程一次都不会被插件系统唤醒（docs/plugin-system/12-performance.md §1）。
    void enableMainThreadSignal();

    // 有主线程待办时被置位的等待句柄（Windows 上是 HANDLE）。
    // 从未加载过插件则返回 nullptr，调用方据此退回无超时等待。
    [[nodiscard]] void* mainThreadWorkWaitHandle() noexcept;

    // 断开某插件的全部订阅并等待其 in-flight 回调返回。
    // 必须在 on_unload 之前调用，见 docs/plugin-system/03-abi-reference.md §5.1 第 1~3 步。
    void detachSubscriber(mcdk_handle owner);

    // 停掉派发线程并清空全部订阅。进程收尾时调用。
    void shutdownEventBus();

} // namespace mcdk::plugin_host

#ifndef MCDK_ENABLE_PLUGINS
#define MCDK_ENABLE_PLUGINS 1
#endif

#if MCDK_ENABLE_PLUGINS

// 发射点的唯一写法。禁止手写 if + dispatch —— 手写会逐渐分化，几年后没人
// 知道哪些发射点还是安全的（12-performance.md §2）。
#define MCDK_EMIT(eventId, makePayload)                                                                                \
    do {                                                                                                               \
        if (::mcdk::plugin_host::hasSubscribers(eventId)) [[unlikely]] {                                               \
            ::mcdk::plugin_host::dispatch((eventId), (makePayload));                                                   \
        }                                                                                                              \
    } while (0)

// 可否决事件。展开为一个表达式，值为 true 表示被否决。
#define MCDK_EMIT_VETOABLE(eventId, makePayload)                                                                       \
    (::mcdk::plugin_host::hasSubscribers(eventId) ? ::mcdk::plugin_host::dispatchVetoable((eventId), (makePayload))    \
                                                  : false)

#else

// 插件系统在本次构建中被关闭。发射点必须在预处理阶段就完全消失，
// 连订阅计数的那一次原子读也不留——这组宏就是 12-performance.md §6 里 A 组的定义。
#define MCDK_EMIT(eventId, makePayload) ((void)0)
#define MCDK_EMIT_VETOABLE(eventId, makePayload) (false)

#endif
