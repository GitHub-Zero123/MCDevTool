#pragma once
// 事件总线的热路径部分。
// 零插件开销契约（docs/plugin-system/12-performance.md §1）：未加载任何插件时，
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <type_traits>

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
    // payload 工厂的临时字符串寄存处。
    // payload 里的 mcdk_str 是借用的，而工厂是个返回 payload 的 lambda——它内部
    class PayloadArena {
    public:
        [[nodiscard]] mcdk_str hold(std::string text) {
            mStorage.push_back(std::move(text));
            const auto& held = mStorage.back();
            return mcdk_str{held.data(), held.size()};
        }

    private:
        // deque 而非 vector：hold 会把指针交出去，追加不能让它失效。
        std::deque<std::string> mStorage;
    };

    // 工厂可接收无参或 arena 参数，后者用于构造字符串 payload。
    template <class Factory>
    void dispatch(EventId id, Factory&& makePayload) {
        if constexpr (std::is_invocable_v<Factory&, PayloadArena&>) {
            PayloadArena arena;
            auto         payload = makePayload(arena);
            dispatchRaw(id, &payload, static_cast<std::uint32_t>(sizeof(payload)));
        } else {
            auto payload = makePayload();
            dispatchRaw(id, &payload, static_cast<std::uint32_t>(sizeof(payload)));
        }
    }

    // 返回 true 表示被某个 SYNC 订阅者否决。QUEUED 订阅者无法否决——它们是
    // 异步的，等不到结果。
    template <class Factory>
    [[nodiscard]] bool dispatchVetoable(EventId id, Factory&& makePayload) {
        // 必须写 else：没有 else 的话后面那两行仍会被实例化，
        // 带 arena 参数的工厂就会在那里报「无匹配的调用」。
        if constexpr (std::is_invocable_v<Factory&, PayloadArena&>) {
            PayloadArena arena;
            auto         payload = makePayload(arena);
            return dispatchRawVetoable(id, &payload, static_cast<std::uint32_t>(sizeof(payload)));
        } else {
            auto payload = makePayload();
            return dispatchRawVetoable(id, &payload, static_cast<std::uint32_t>(sizeof(payload)));
        }
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
