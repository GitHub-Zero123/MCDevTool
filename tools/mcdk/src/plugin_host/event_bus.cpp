#include <mcdk/plugin_host/events.hpp>

#include <mcdk/plugin_host/guard.hpp>

#include "registry.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mcdk::plugin_host {

    std::array<std::atomic<std::uint32_t>, kEventCount> gSubscriberCount{};

    namespace {

        constexpr std::size_t kMaxQueuedEvents = 4096;
        // payload 布局表
        // QUEUED 派发必须深拷贝：发射方的字符串存储在 MCDK_EMIT 所在作用域
        constexpr std::uint16_t kStrGameLaunchBefore[] = {
            static_cast<std::uint16_t>(offsetof(mcdk_ev_game_launch_before, exe_path)),
            static_cast<std::uint16_t>(offsetof(mcdk_ev_game_launch_before, dev_config_path)),
        };
        constexpr std::uint16_t kStrGameLaunchFinish[] = {
            static_cast<std::uint16_t>(offsetof(mcdk_ev_game_launch_finish, exe_path)),
        };
        constexpr std::uint16_t kStrLogLine[] = {
            static_cast<std::uint16_t>(offsetof(mcdk_ev_log_line, text)),
        };

        struct EventTraits {
            std::uint32_t        size;
            const std::uint16_t* strings;
            std::uint16_t        stringCount;
            // 被否决时这个事件是否就此作废。
            // `.before` 类事件否决即不会发生，再投给异步订阅者只会让对方
            bool vetoCancelsEvent;
            // 订阅一律按 SYNC 处理，忽略订阅方要的模式。注册窗口类事件必须如此：
            // 宿主发完就封存注册表，异步 handler 醒来时窗口早关了，而且是静默关的。
            bool syncOnly;
        };

        constexpr EventTraits kTraits[] = {
            /* McpRegisterBefore     */ {sizeof(mcdk_ev_mcp_register), nullptr, 0, false, true},
            /* McpRegisterFinish     */ {sizeof(mcdk_ev_mcp_register), nullptr, 0, false, true},
            /* GameLaunchBefore      */ {sizeof(mcdk_ev_game_launch_before), kStrGameLaunchBefore, 2, true, false},
            /* GameLaunchFinish      */ {sizeof(mcdk_ev_game_launch_finish), kStrGameLaunchFinish, 1, false, false},
            /* GameExit              */ {sizeof(mcdk_ev_game_exit), nullptr, 0, false, false},
            /* GameStateChanged      */ {sizeof(mcdk_ev_game_state), nullptr, 0, false, false},
            /* LogLine               */ {sizeof(mcdk_ev_log_line), kStrLogLine, 1, false, false},
            /* LogError              */ {sizeof(mcdk_ev_log_line), kStrLogLine, 1, false, false},
            /* IpcClientConnected    */ {sizeof(mcdk_ev_ipc_client), nullptr, 0, false, false},
            /* IpcClientDisconnected */ {sizeof(mcdk_ev_ipc_client), nullptr, 0, false, false},
        };
        static_assert(std::size(kTraits) == kEventCount, "每新增一个事件都必须在这里登记 payload 布局与否决语义");
        // 把 payload 和字符串打包成自包含 blob，先存偏移，最后再还原指针。
        void pack(EventId id, const void* payload, std::uint32_t payloadSize, std::vector<unsigned char>& blob) {
            const auto&          layout = kTraits[static_cast<std::size_t>(id)];
            const auto* const    bytes  = static_cast<const unsigned char*>(payload);
            blob.assign(bytes, bytes + payloadSize);
            for (std::uint16_t index = 0; index < layout.stringCount; ++index) {
                const std::uint16_t fieldOffset = layout.strings[index];
                mcdk_str            field{};
                std::memcpy(&field, bytes + fieldOffset, sizeof(field));

                const std::size_t textOffset = blob.size();
                if (field.len != 0 && field.ptr != nullptr) {
                    blob.insert(blob.end(), field.ptr, field.ptr + field.len);
                }
                // insert 可能重分配，所以偏移只能在追加完成后写回。
                mcdk_str relocatable{};
                relocatable.ptr = reinterpret_cast<const char*>(static_cast<std::uintptr_t>(textOffset));
                relocatable.len = field.len;
                std::memcpy(blob.data() + fieldOffset, &relocatable, sizeof(relocatable));
            }
        }

        // 把 pack 存下的偏移还原成真指针。只能在 blob 不会再搬家之后调用。
        void relocate(EventId id, std::vector<unsigned char>& blob) {
            const auto& layout = kTraits[static_cast<std::size_t>(id)];
            for (std::uint16_t index = 0; index < layout.stringCount; ++index) {
                const std::uint16_t fieldOffset = layout.strings[index];
                mcdk_str            field{};
                std::memcpy(&field, blob.data() + fieldOffset, sizeof(field));
                const auto textOffset = reinterpret_cast<std::uintptr_t>(field.ptr);
                field.ptr = field.len != 0 ? reinterpret_cast<const char*>(blob.data() + textOffset) : nullptr;
                std::memcpy(blob.data() + fieldOffset, &field, sizeof(field));
            }
        }

        // ------------------------------------------------------------------
        // 订阅表
        // ------------------------------------------------------------------
        struct Subscription {
            mcdk_handle        token    = 0;
            mcdk_handle        owner    = 0;
            EventId            event    = EventId::Count;
            mcdk_dispatch_mode mode     = MCDK_DISPATCH_QUEUED;
            std::int32_t       priority = 0;
            mcdk_event_handler handler  = nullptr;
            void*              user     = nullptr;
            // 锁内写、派发循环在锁外读，必须是原子的。
            std::atomic<bool> alive{false};
            // detach 要等它归零才能放心调 on_unload，否则事件会打进正在析构的插件。
            std::atomic<int> inFlight{0};
        };

        struct QueuedEvent {
            EventId                    event       = EventId::Count;
            std::uint32_t              payloadSize = 0;
            std::vector<unsigned char> blob;
        };

        struct Bus {
            std::mutex mutex;
            // deque + 槽位复用：派发会把裸指针带出锁外使用，容器不能搬动元素。
            std::deque<Subscription> subscriptions;
            mcdk_handle              nextToken = 1;

            // ---- QUEUED 派发线程（惰性创建）----
            std::thread             worker;
            std::deque<QueuedEvent> queue;
            std::condition_variable queueReady;
            bool                    stopping      = false;
            bool                    workerLive    = false;
            std::uint64_t           droppedEvents = 0;
            bool                    dropWarned    = false;

            // ---- detach 等待 in-flight ----
            std::condition_variable inFlightDone;
        };

        Bus& bus() {
            static Bus instance;
            return instance;
        }

        struct NameEntry {
            EventId          id;
            std::string_view name;
        };

        constexpr NameEntry kNames[] = {
            {EventId::McpRegisterBefore, MCDK_EVENT_MCP_REGISTER_BEFORE},
            {EventId::McpRegisterFinish, MCDK_EVENT_MCP_REGISTER_FINISH},
            {EventId::GameLaunchBefore, MCDK_EVENT_GAME_LAUNCH_BEFORE},
            {EventId::GameLaunchFinish, MCDK_EVENT_GAME_LAUNCH_FINISH},
            {EventId::GameExit, MCDK_EVENT_GAME_EXIT},
            {EventId::GameStateChanged, MCDK_EVENT_GAME_STATE_CHANGED},
            {EventId::LogLine, MCDK_EVENT_LOG_LINE},
            {EventId::LogError, MCDK_EVENT_LOG_ERROR},
            {EventId::IpcClientConnected, MCDK_EVENT_IPC_CLIENT_CONNECTED},
            {EventId::IpcClientDisconnected, MCDK_EVENT_IPC_CLIENT_DISCONNECTED},
        };
        static_assert(std::size(kNames) == kEventCount, "每新增一个事件都必须在这里登记名字");

        void warnOnce(bool& flag, const std::string& message) {
            if (flag) {
                return;
            }
            flag               = true;
            const auto& output = detail::outputCallback();
            if (output) {
                output(message, ConsoleColor::Yellow);
            }
        }

        enum class Want { Sync, Async };

        // 取一份当前存活订阅者的快照，并把 in-flight 计数先加上。
        // 回调必须在锁外调用：插件可能反过来调用宿主接口。
        void acquire(EventId id, Want want, std::vector<Subscription*>& out) {
            out.clear();
            {
                const std::lock_guard lock(bus().mutex);
                for (auto& subscription : bus().subscriptions) {
                    if (!subscription.alive.load(std::memory_order_relaxed) || subscription.event != id) {
                        continue;
                    }
                    const bool isSync = subscription.mode == MCDK_DISPATCH_SYNC;
                    if ((want == Want::Sync) != isSync) {
                        continue;
                    }
                    subscription.inFlight.fetch_add(1, std::memory_order_acq_rel);
                    out.push_back(&subscription);
                }
            }
            std::stable_sort(out.begin(), out.end(), [](const Subscription* left, const Subscription* right) {
                return left->priority < right->priority;
            });
        }

        void releaseOne(Subscription* subscription) {
            subscription->inFlight.fetch_sub(1, std::memory_order_acq_rel);
            bus().inFlightDone.notify_all();
        }

        void release(const std::vector<Subscription*>& taken) {
            for (auto* subscription : taken) {
                subscription->inFlight.fetch_sub(1, std::memory_order_acq_rel);
            }
            bus().inFlightDone.notify_all();
        }

        mcdk_event makeEvent(EventId id, const void* payload, std::uint32_t payloadSize) {
            mcdk_event event{};
            event.struct_size     = static_cast<std::uint32_t>(sizeof(mcdk_event));
            event.event_id        = toAbiEventId(id);
            event.payload_version = 1;
            event.payload_size    = payloadSize;
            event.payload         = payload;
            return event;
        }

        void workerLoop() {
            std::vector<Subscription*> taken;
            std::vector<Subscription*> toRelease;
            for (;;) {
                QueuedEvent item;
                {
                    std::unique_lock lock(bus().mutex);
                    bus().queueReady.wait(lock, [] { return bus().stopping || !bus().queue.empty(); });
                    if (bus().stopping && bus().queue.empty()) {
                        return;
                    }
                    item = std::move(bus().queue.front());
                    bus().queue.pop_front();
                }
                acquire(item.event, Want::Async, taken);
                toRelease.clear();

                // blob 已经落到 item 上不会再搬家，现在才能把偏移还原成真指针。
                relocate(item.event, item.blob);
                const auto event = makeEvent(item.event, item.blob.data(), item.payloadSize);

                for (auto* subscription : taken) {
                    if (!subscription->alive.load(std::memory_order_acquire)) {
                        toRelease.push_back(subscription);
                        continue;
                    }
                    // 插件侧的异常由 SDK 屏障吃掉，这里拿到的只会是返回值。
                    (void)subscription->handler(&event, subscription->user);
                    toRelease.push_back(subscription);
                }
                release(toRelease);
            }
        }

        void ensureWorker() {
            // 调用者持有 bus().mutex。
            if (bus().workerLive) {
                return;
            }
            bus().workerLive = true;
            bus().worker     = std::thread(workerLoop);
        }

        void enqueue(EventId id, const void* payload, std::uint32_t payloadSize) {
            QueuedEvent item;
            item.event       = id;
            item.payloadSize = payloadSize;
            // 深拷贝在锁外做：pack 会分配，没必要占着派发锁。
            pack(id, payload, payloadSize, item.blob);

            const std::lock_guard lock(bus().mutex);
            if (bus().queue.size() >= kMaxQueuedEvents) {
                // 有界队列：宁可丢事件，也不能让插件拖住游戏日志管道。
                ++bus().droppedEvents;
                warnOnce(bus().dropWarned, "[Plugin] 事件队列已满，开始丢弃事件：某个插件的处理器过慢");
                return;
            }
            bus().queue.push_back(std::move(item));
            bus().queueReady.notify_one();
        }

        // 异步（QUEUED）订阅者的计数。dispatchRaw 据此决定要不要付深拷贝的钱。
        std::array<std::atomic<std::uint32_t>, kEventCount> gAsyncSubscriberCount{};

        bool hasAsyncSubscribers(EventId id) noexcept {
            return gAsyncSubscriberCount[static_cast<std::size_t>(id)].load(std::memory_order_relaxed) != 0;
        }

        bool payloadSizeMatches(EventId id, std::uint32_t payloadSize) {
            const auto& layout = kTraits[static_cast<std::size_t>(id)];
            if (payloadSize == layout.size) {
                return true;
            }
            // 新增事件却漏了布局表登记，或者 payload 结构体改了尺寸没同步。
            // 宁可不发也不能按错误的布局去读字符串字段。
            static bool warned = false;
            warnOnce(
                warned,
                "[Plugin] 事件 " + std::string(eventName(id)) + " 的 payload 尺寸与事件表不符，已拒绝发射"
            );
            return false;
        }

        bool anyInFlight(mcdk_handle owner) {
            for (const auto& subscription : bus().subscriptions) {
                if (subscription.owner == owner && subscription.inFlight.load(std::memory_order_acquire) > 0) {
                    return true;
                }
            }
            return false;
        }

    } // namespace

    std::uint32_t toAbiEventId(EventId id) noexcept {
        // +1：ABI 上 0 保留给「未知事件名」。
        return static_cast<std::uint32_t>(id) + 1;
    }

    bool fromAbiEventId(std::uint32_t abiId, EventId& out) noexcept {
        if (abiId == kInvalidAbiEventId || abiId > kEventCount) {
            return false;
        }
        out = static_cast<EventId>(abiId - 1);
        return true;
    }

    std::uint32_t resolveEventName(std::string_view name) noexcept {
        for (const auto& entry : kNames) {
            if (entry.name == name) {
                return toAbiEventId(entry.id);
            }
        }
        return kInvalidAbiEventId;
    }

    std::string_view eventName(EventId id) noexcept {
        const auto index = static_cast<std::size_t>(id);
        return index < kEventCount ? kNames[index].name : std::string_view{};
    }

    void dispatchRaw(EventId id, const void* payload, std::uint32_t payloadSize) {
        if (!payloadSizeMatches(id, payloadSize)) {
            return;
        }
        // SYNC 处理器可能嵌套派发，因此不复用 thread_local 缓冲。
            std::vector<Subscription*> taken;
        acquire(id, Want::Sync, taken);
        if (!taken.empty()) {
            const auto event = makeEvent(id, payload, payloadSize);
            for (auto* subscription : taken) {
                if (!subscription->alive.load(std::memory_order_acquire)) {
                    continue;
                }
                if (subscription->handler(&event, subscription->user) == MCDK_EVENT_STOP) {
                    break;
                }
            }
            release(taken);
        }
        // 没有异步订阅者就不要付深拷贝和入队的钱——更要紧的是，没有异步订阅者时
        // 派发线程根本不存在，无条件入队会让队列涨满 4096 然后报出一句
        if (hasAsyncSubscribers(id)) {
            enqueue(id, payload, payloadSize);
        }
    }

    bool dispatchRawVetoable(EventId id, const void* payload, std::uint32_t payloadSize) {
        if (!payloadSizeMatches(id, payloadSize)) {
            return false;
        }
        bool                       vetoed = false;
        std::vector<Subscription*> taken;
        acquire(id, Want::Sync, taken);
        if (!taken.empty()) {
            const auto event = makeEvent(id, payload, payloadSize);
            for (auto* subscription : taken) {
                if (!subscription->alive.load(std::memory_order_acquire)) {
                    continue;
                }
                const auto result = subscription->handler(&event, subscription->user);
                if (result == MCDK_EVENT_VETO) {
                    vetoed = true;
                    break;
                }
                if (result == MCDK_EVENT_STOP) {
                    break;
                }
            }
            release(taken);
        }
        // 是否因否决而不再投递给异步订阅者，由事件自己的语义决定。
        const bool cancelled = vetoed && kTraits[static_cast<std::size_t>(id)].vetoCancelsEvent;
        if (!cancelled && hasAsyncSubscribers(id)) {
            enqueue(id, payload, payloadSize);
        }
        return vetoed;
    }

    mcdk_handle subscribeEvent(
        mcdk_handle        owner,
        EventId            id,
        mcdk_dispatch_mode mode,
        std::int32_t       priority,
        mcdk_event_handler handler,
        void*              user
    ) {
        if (handler == nullptr || id >= EventId::Count) {
            return 0;
        }
        if (kTraits[static_cast<std::size_t>(id)].syncOnly) {
            mode = MCDK_DISPATCH_SYNC;
        }
        const std::lock_guard lock(bus().mutex);
        const auto            token = bus().nextToken++;
        // 槽位复用：退订只置 alive=false，不删元素（派发会把裸指针带出锁外）。
        // 不复用的话，反复订阅/退订的 loader 型插件会让派发的线性扫描无限变长。
        Subscription* entry = nullptr;
        for (auto& candidate : bus().subscriptions) {
            if (!candidate.alive.load(std::memory_order_relaxed)
                && candidate.inFlight.load(std::memory_order_acquire) == 0) {
                entry = &candidate;
                break;
            }
        }
        if (entry == nullptr) {
            entry = &bus().subscriptions.emplace_back();
        }
        entry->token    = token;
        entry->owner    = owner;
        entry->event    = id;
        entry->mode     = mode;
        entry->priority = priority;
        entry->handler  = handler;
        entry->user     = user;

        if (mode != MCDK_DISPATCH_SYNC) {
            // 惰性：没有异步订阅就不建线程（12-performance.md §3）。
            ensureWorker();
            gAsyncSubscriberCount[static_cast<std::size_t>(id)].fetch_add(1, std::memory_order_relaxed);
        }
        gSubscriberCount[static_cast<std::size_t>(id)].fetch_add(1, std::memory_order_relaxed);
        // alive 最后置位：在此之前这条订阅对派发不可见。
        entry->alive.store(true, std::memory_order_release);
        return token;
    }

    namespace {
        // 调用者持有 bus().mutex。
        void retireSubscription(Subscription& subscription) {
            subscription.alive.store(false, std::memory_order_release);
            const auto index = static_cast<std::size_t>(subscription.event);
            gSubscriberCount[index].fetch_sub(1, std::memory_order_relaxed);
            if (subscription.mode != MCDK_DISPATCH_SYNC) {
                gAsyncSubscriberCount[index].fetch_sub(1, std::memory_order_relaxed);
            }
        }
    } // namespace

    void unsubscribeEvent(mcdk_handle owner, mcdk_handle token) {
        const std::lock_guard lock(bus().mutex);
        for (auto& subscription : bus().subscriptions) {
            if (subscription.token != token || subscription.owner != owner
                || !subscription.alive.load(std::memory_order_relaxed)) {
                continue;
            }
            retireSubscription(subscription);
            return;
        }
    }

    void detachSubscriber(mcdk_handle owner) {
        {
            // 第 1、3 步：停止派发并注销订阅。
            const std::lock_guard lock(bus().mutex);
            for (auto& subscription : bus().subscriptions) {
                if (subscription.owner != owner || !subscription.alive.load(std::memory_order_relaxed)) {
                    continue;
                }
                retireSubscription(subscription);
            }
        }
        // 第 2 步：等待 in-flight 回调返回。必须在 on_unload 之前完成，
        // 否则事件会打进正在析构的插件对象（03-abi-reference.md §5.1）。
        std::unique_lock lock(bus().mutex);
        bus().inFlightDone.wait(lock, [owner] { return !anyInFlight(owner); });
    }

    void shutdownEventBus() {
        std::thread worker;
        {
            const std::lock_guard lock(bus().mutex);
            if (bus().workerLive) {
                bus().stopping = true;
                bus().queueReady.notify_all();
                worker = std::move(bus().worker);
            }
        }
        if (worker.joinable()) {
            worker.join();
        }
        const std::lock_guard lock(bus().mutex);
        bus().workerLive = false;
        bus().stopping   = false;
        bus().subscriptions.clear();
        bus().queue.clear();
        for (auto& counter : gSubscriberCount) {
            counter.store(0, std::memory_order_relaxed);
        }
        for (auto& counter : gAsyncSubscriberCount) {
            counter.store(0, std::memory_order_relaxed);
        }
    }

} // namespace mcdk::plugin_host
