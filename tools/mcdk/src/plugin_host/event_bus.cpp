#include <mcdk/plugin_host/events.hpp>

#include <mcdk/plugin_host/guard.hpp>

#include "registry.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace mcdk::plugin_host {

    std::array<std::atomic<std::uint32_t>, kEventCount> gSubscriberCount{};

    namespace {

        constexpr std::size_t kMaxQueuedEvents = 4096;
        // payload 都是小 POD，固定大小的内联缓冲省掉每次入队的一次分配。
        constexpr std::size_t kMaxPayloadSize = 128;

        struct Subscription {
            mcdk_handle        token    = 0;
            mcdk_handle        owner    = 0;
            EventId            event    = EventId::Count;
            mcdk_dispatch_mode mode     = MCDK_DISPATCH_QUEUED;
            std::int32_t       priority = 0;
            mcdk_event_handler handler  = nullptr;
            void*              user     = nullptr;
            bool               alive    = true;
            // detach 要等它归零才能放心调 on_unload，否则事件会打进正在析构的插件。
            std::atomic<int> inFlight{0};
        };

        struct QueuedEvent {
            EventId       event       = EventId::Count;
            std::uint32_t payloadSize = 0;
            // 深拷贝：QUEUED 模式下发射方的栈帧早就没了。
            alignas(std::max_align_t) unsigned char payload[kMaxPayloadSize]{};
        };

        struct MainThreadWork {
            void (*MCDK_CALL fn)(void*) = nullptr;
            void* user                  = nullptr;
        };

        struct Bus {
            std::mutex mutex;
            // deque：dispatch 会把裸指针带出锁外使用，追加不能让它失效。
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

            // ---- 主线程队列 ----
            std::mutex                  mainMutex;
            std::vector<MainThreadWork> mainWork;
            std::atomic<bool>           mainWorkPending{false};

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
            {EventId::LogLine, MCDK_EVENT_LOG_LINE},
            {EventId::LogError, MCDK_EVENT_LOG_ERROR},
            {EventId::IpcClientConnected, MCDK_EVENT_IPC_CLIENT_CONNECTED},
            {EventId::IpcClientDisconnected, MCDK_EVENT_IPC_CLIENT_DISCONNECTED},
        };
        static_assert(std::size(kNames) == kEventCount, "每新增一个事件都必须在这里登记名字");

        // 取一份当前存活订阅者的快照，并把 in-flight 计数先加上。
        // 回调必须在锁外调用：插件可能反过来调用宿主接口。
        std::vector<Subscription*> acquire(EventId id, bool syncOnly) {
            std::vector<Subscription*> result;
            {
                const std::lock_guard lock(bus().mutex);
                for (auto& subscription : bus().subscriptions) {
                    if (!subscription.alive || subscription.event != id) {
                        continue;
                    }
                    if (syncOnly && subscription.mode != MCDK_DISPATCH_SYNC) {
                        continue;
                    }
                    subscription.inFlight.fetch_add(1, std::memory_order_acq_rel);
                    result.push_back(&subscription);
                }
            }
            std::stable_sort(result.begin(), result.end(), [](const Subscription* left, const Subscription* right) {
                return left->priority < right->priority;
            });
            return result;
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
            for (;;) {
                QueuedEvent item;
                {
                    std::unique_lock lock(bus().mutex);
                    bus().queueReady.wait(lock, [] { return bus().stopping || !bus().queue.empty(); });
                    if (bus().stopping && bus().queue.empty()) {
                        return;
                    }
                    item = bus().queue.front();
                    bus().queue.pop_front();
                }
                const auto taken = acquire(item.event, /*syncOnly=*/false);
                const auto event = makeEvent(item.event, item.payload, item.payloadSize);
                for (auto* subscription : taken) {
                    if (subscription->mode == MCDK_DISPATCH_SYNC || !subscription->alive) {
                        continue; // SYNC 的已在发射线程上跑过
                    }
                    // 插件侧的异常由 SDK 屏障吃掉，这里拿到的只会是返回值。
                    (void)subscription->handler(&event, subscription->user);
                }
                release(taken);
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
            if (payloadSize > kMaxPayloadSize) {
                // 编码错误而非运行期状况：payload 都是小 POD。
                return;
            }
            const std::lock_guard lock(bus().mutex);
            if (bus().queue.size() >= kMaxQueuedEvents) {
                // 有界队列：宁可丢事件，也不能让插件拖住游戏日志管道。
                ++bus().droppedEvents;
                if (!bus().dropWarned) {
                    bus().dropWarned   = true;
                    const auto& output = detail::outputCallback();
                    if (output) {
                        output("[Plugin] 事件队列已满，开始丢弃事件：某个插件的处理器过慢", ConsoleColor::Yellow);
                    }
                }
                return;
            }
            QueuedEvent item;
            item.event       = id;
            item.payloadSize = payloadSize;
            std::memcpy(item.payload, payload, payloadSize);
            bus().queue.push_back(item);
            bus().queueReady.notify_one();
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
        // SYNC 的就地跑，其余排队。
        const auto taken = acquire(id, /*syncOnly=*/true);
        if (!taken.empty()) {
            const auto event = makeEvent(id, payload, payloadSize);
            for (auto* subscription : taken) {
                if (!subscription->alive) {
                    continue;
                }
                if (subscription->handler(&event, subscription->user) == MCDK_EVENT_STOP) {
                    break;
                }
            }
            release(taken);
        }
        enqueue(id, payload, payloadSize);
    }

    bool dispatchRawVetoable(EventId id, const void* payload, std::uint32_t payloadSize) {
        bool       vetoed = false;
        const auto taken  = acquire(id, /*syncOnly=*/true);
        if (!taken.empty()) {
            const auto event = makeEvent(id, payload, payloadSize);
            for (auto* subscription : taken) {
                if (!subscription->alive) {
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
        // QUEUED 订阅者收不到否决权，但仍然应该知道这件事发生过。
        enqueue(id, payload, payloadSize);
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
        const std::lock_guard lock(bus().mutex);
        const auto            token = bus().nextToken++;
        auto&                 entry = bus().subscriptions.emplace_back();
        entry.token                 = token;
        entry.owner                 = owner;
        entry.event                 = id;
        entry.mode                  = mode;
        entry.priority              = priority;
        entry.handler               = handler;
        entry.user                  = user;
        entry.alive                 = true;

        if (mode == MCDK_DISPATCH_QUEUED) {
            // 惰性：没有 QUEUED 订阅就不建线程（12-performance.md §3）。
            ensureWorker();
        }
        gSubscriberCount[static_cast<std::size_t>(id)].fetch_add(1, std::memory_order_relaxed);
        return token;
    }

    void unsubscribeEvent(mcdk_handle owner, mcdk_handle token) {
        const std::lock_guard lock(bus().mutex);
        for (auto& subscription : bus().subscriptions) {
            if (subscription.token != token || subscription.owner != owner || !subscription.alive) {
                continue;
            }
            subscription.alive = false;
            gSubscriberCount[static_cast<std::size_t>(subscription.event)].fetch_sub(1, std::memory_order_relaxed);
            return;
        }
    }

    void postMainThreadWork(void(MCDK_CALL* fn)(void*), void* user) {
        if (fn == nullptr) {
            return;
        }
        {
            const std::lock_guard lock(bus().mainMutex);
            bus().mainWork.push_back(MainThreadWork{fn, user});
        }
        bus().mainWorkPending.store(true, std::memory_order_release);
    }

    void pumpMainThreadWork() {
        // 无待办时就是一次原子读 + 分支。
        if (!bus().mainWorkPending.load(std::memory_order_acquire)) {
            return;
        }
        std::vector<MainThreadWork> work;
        {
            const std::lock_guard lock(bus().mainMutex);
            work.swap(bus().mainWork);
            bus().mainWorkPending.store(false, std::memory_order_release);
        }
        for (const auto& item : work) {
            // 插件侧已有屏障，这里只兜宿主自身的意外。
            guardVoid([&item] { item.fn(item.user); });
        }
    }

    void detachSubscriber(mcdk_handle owner) {
        std::unique_lock lock(bus().mutex);
        // 第 1、3 步：停止派发并注销订阅。
        for (auto& subscription : bus().subscriptions) {
            if (subscription.owner != owner || !subscription.alive) {
                continue;
            }
            subscription.alive = false;
            gSubscriberCount[static_cast<std::size_t>(subscription.event)].fetch_sub(1, std::memory_order_relaxed);
        }
        // 第 2 步：等待 in-flight 回调返回。必须在 on_unload 之前完成，
        // 否则事件会打进正在析构的插件对象（03-abi-reference.md §5.1）。
        bus().inFlightDone.wait(lock, [owner] {
            for (const auto& subscription : bus().subscriptions) {
                if (subscription.owner == owner && subscription.inFlight.load(std::memory_order_acquire) > 0) {
                    return false;
                }
            }
            return true;
        });
    }

    void shutdownEventBus() {
        std::thread worker;
        {
            const std::lock_guard lock(bus().mutex);
            if (!bus().workerLive) {
                bus().subscriptions.clear();
                return;
            }
            bus().stopping = true;
            bus().queueReady.notify_all();
            worker = std::move(bus().worker);
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
    }

} // namespace mcdk::plugin_host
