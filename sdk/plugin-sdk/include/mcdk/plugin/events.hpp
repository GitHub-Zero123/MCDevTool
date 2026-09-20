#pragma once

//
// 事件订阅的 C++ 封装。
//
// 用户写 ctx.events().on<ev::GameLaunchFinish>([](const auto& e){ ... })，
// 拿到的是一个有具名字段的 C++ 结构体；ABI 上的 mcdk_event、payload 指针、
// struct_size 探测、以及异常屏障，都由这一层消化掉。
//

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "abi/events.h"
#include "abi/iface/events.h"
#include "detail/abi_bridge.hpp"
#include "detail/barrier.hpp"

namespace mcdk {

    enum class Dispatch {
        // 默认。payload 已深拷贝，回调跑在插件专用线程上，慢了也不拖住宿主。
        Queued,
        // 发射线程内同步调用，可否决。必须极快：它串在宿主热路径上。
        Sync,
        // 投递到主线程。
        Main
    };

    enum class EventResult { Continue, Stop, Veto };

    using SubscriptionToken = mcdk_handle;

    // ---------------------------------------------------------------
    // 类型化事件
    // ---------------------------------------------------------------
    namespace ev {

        struct McpRegisterBefore {
            using Payload                      = mcdk_ev_mcp_register;
            static constexpr auto    abiName   = MCDK_EVENT_MCP_REGISTER_BEFORE;
            std::uint32_t            toolCount = 0;
            static McpRegisterBefore from(const Payload& raw) { return {raw.tool_count}; }
        };

        struct McpRegisterFinish {
            using Payload                      = mcdk_ev_mcp_register;
            static constexpr auto    abiName   = MCDK_EVENT_MCP_REGISTER_FINISH;
            std::uint32_t            toolCount = 0;
            static McpRegisterFinish from(const Payload& raw) { return {raw.tool_count}; }
        };

        struct GameLaunchBefore {
            using Payload                 = mcdk_ev_game_launch_before;
            static constexpr auto abiName = MCDK_EVENT_GAME_LAUNCH_BEFORE;
            // 借用：回调返回即失效，要留请自行拷贝。
            std::string_view        exePath;
            std::string_view        devConfigPath;
            static GameLaunchBefore from(const Payload& raw) {
                return {detail::toView(raw.exe_path), detail::toView(raw.dev_config_path)};
            }
        };

        struct GameLaunchFinish {
            using Payload                   = mcdk_ev_game_launch_finish;
            static constexpr auto   abiName = MCDK_EVENT_GAME_LAUNCH_FINISH;
            std::uint32_t           pid     = 0;
            std::string_view        exePath;
            static GameLaunchFinish from(const Payload& raw) { return {raw.pid, detail::toView(raw.exe_path)}; }
        };

        struct GameExit {
            using Payload                  = mcdk_ev_game_exit;
            static constexpr auto abiName  = MCDK_EVENT_GAME_EXIT;
            std::uint32_t         pid      = 0;
            std::int32_t          exitCode = 0;
            static GameExit       from(const Payload& raw) { return {raw.pid, raw.exit_code}; }
        };

        struct LogLine {
            using Payload                     = mcdk_ev_log_line;
            static constexpr auto abiName     = MCDK_EVENT_LOG_LINE;
            std::int64_t          timestampMs = 0;
            std::string_view      text;
            static LogLine        from(const Payload& raw) { return {raw.timestamp_ms, detail::toView(raw.text)}; }
        };

        struct LogError {
            using Payload                     = mcdk_ev_log_line;
            static constexpr auto abiName     = MCDK_EVENT_LOG_ERROR;
            std::int64_t          timestampMs = 0;
            std::string_view      text;
            static LogError       from(const Payload& raw) { return {raw.timestamp_ms, detail::toView(raw.text)}; }
        };

        struct IpcClientConnected {
            using Payload                         = mcdk_ev_ipc_client;
            static constexpr auto     abiName     = MCDK_EVENT_IPC_CLIENT_CONNECTED;
            std::uint32_t             clientCount = 0;
            static IpcClientConnected from(const Payload& raw) { return {raw.client_count}; }
        };

        struct IpcClientDisconnected {
            using Payload                            = mcdk_ev_ipc_client;
            static constexpr auto        abiName     = MCDK_EVENT_IPC_CLIENT_DISCONNECTED;
            std::uint32_t                clientCount = 0;
            static IpcClientDisconnected from(const Payload& raw) { return {raw.client_count}; }
        };

    } // namespace ev

    // ---------------------------------------------------------------
    // Events
    // ---------------------------------------------------------------
    class Events {
    public:
        Events() = default;

        Events(mcdk_handle self, const mcdk_iface_events* table) noexcept : mSelf(self), mTable(table) {}

        [[nodiscard]] bool available() const noexcept { return mTable != nullptr; }

        // 默认 Queued。处理器可以返回 void 或 EventResult。
        template <class Event, class Handler>
        SubscriptionToken on(Handler&& handler) {
            return on<Event>(Dispatch::Queued, std::forward<Handler>(handler));
        }

        template <class Event, class Handler>
        SubscriptionToken on(Dispatch mode, Handler&& handler, std::int32_t priority = 0) {
            if (!detail::ifaceHas(mTable, &mcdk_iface_events::subscribe)) {
                return 0;
            }
            const auto id = resolve(Event::abiName);
            if (id == 0) {
                // 该宿主不认识这个事件：优雅降级，不是错误。
                return 0;
            }

            using Stored      = std::decay_t<Handler>;
            auto       holder = std::make_unique<Holder<Stored>>(std::forward<Handler>(handler));
            void*      user   = holder.get();
            const auto token  = mTable->subscribe(mSelf, id, toAbi(mode), priority, &trampoline<Event, Stored>, user);
            if (token == 0) {
                return 0;
            }
            // 闭包必须活过订阅；由本对象持有，随插件实例一起销毁。
            mHolders.push_back(std::move(holder));
            return token;
        }

        void off(SubscriptionToken token) const noexcept {
            if (detail::ifaceHas(mTable, &mcdk_iface_events::unsubscribe) && token != 0) {
                mTable->unsubscribe(mSelf, token);
            }
        }

        [[nodiscard]] std::uint32_t resolve(std::string_view name) const noexcept {
            if (!detail::ifaceHas(mTable, &mcdk_iface_events::resolve)) {
                return 0;
            }
            return mTable->resolve(mSelf, detail::toAbi(name));
        }

    private:
        struct HolderBase {
            virtual ~HolderBase() = default;
        };

        template <class Fn>
        struct Holder : HolderBase {
            explicit Holder(Fn&& value) : fn(std::move(value)) {}
            explicit Holder(const Fn& value) : fn(value) {}
            Fn fn;
        };

        [[nodiscard]] static constexpr mcdk_dispatch_mode toAbi(Dispatch mode) noexcept {
            switch (mode) {
            case Dispatch::Sync:
                return MCDK_DISPATCH_SYNC;
            case Dispatch::Main:
                return MCDK_DISPATCH_MAIN;
            case Dispatch::Queued:
            default:
                return MCDK_DISPATCH_QUEUED;
            }
        }

        [[nodiscard]] static constexpr mcdk_event_result toAbi(EventResult result) noexcept {
            switch (result) {
            case EventResult::Stop:
                return MCDK_EVENT_STOP;
            case EventResult::Veto:
                return MCDK_EVENT_VETO;
            case EventResult::Continue:
            default:
                return MCDK_EVENT_CONTINUE;
            }
        }

        template <class Event, class Fn>
        static mcdk_event_result MCDK_CALL trampoline(const mcdk_event* event, void* user) noexcept {
            // 处理器抛异常一律记为 CONTINUE，绝不是 VETO —— 否则插件里的一个
            // bug 就能让游戏起不来（02-abi-contract.md §4.2）。
            return detail::guard(
                [event, user]() -> mcdk_event_result {
                    if (event == nullptr || event->payload == nullptr) {
                        return MCDK_EVENT_CONTINUE;
                    }
                    using Payload = typename Event::Payload;
                    if (event->payload_size < sizeof(Payload)) {
                        // 宿主比插件旧，payload 缺字段。加载期的 minor 校验本应
                        // 拦住这种组合，这里只是不信任地兜一层。
                        return MCDK_EVENT_CONTINUE;
                    }
                    const auto& raw     = *static_cast<const Payload*>(event->payload);
                    auto&       handler = static_cast<Holder<Fn>*>(user)->fn;
                    const auto  typed   = Event::from(raw);

                    if constexpr (std::is_void_v<decltype(handler(typed))>) {
                        handler(typed);
                        return MCDK_EVENT_CONTINUE;
                    } else {
                        return toAbi(handler(typed));
                    }
                },
                MCDK_EVENT_CONTINUE
            );
        }

        mcdk_handle                              mSelf  = 0;
        const mcdk_iface_events*                 mTable = nullptr;
        std::vector<std::unique_ptr<HolderBase>> mHolders;
    };

} // namespace mcdk
