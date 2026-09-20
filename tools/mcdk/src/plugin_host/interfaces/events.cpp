// mcdk.events/1 的宿主实现。
// 本文件里的每个导出函数都必须经过 guard / guardVoid，没有例外
#include <string_view>

#include <mcdk/plugin/abi/iface/events.h>
#include <mcdk/plugin_host/events.hpp>
#include <mcdk/plugin_host/guard.hpp>

#include "../registry.hpp"

namespace mcdk::plugin_host::detail {

    namespace {

        [[nodiscard]] std::string_view toView(mcdk_str text) noexcept {
            if (text.ptr == nullptr || text.len == 0) {
                return {};
            }
            return std::string_view(text.ptr, text.len);
        }

        std::uint32_t MCDK_CALL eventsResolve(mcdk_handle self, mcdk_str name) noexcept {
            std::uint32_t id = kInvalidAbiEventId;
            guardVoid([&] {
                if (registry().find(self) == nullptr) {
                    return;
                }
                id = resolveEventName(toView(name));
            });
            return id;
        }

        mcdk_handle MCDK_CALL eventsSubscribe(
            mcdk_handle        self,
            std::uint32_t      eventId,
            mcdk_dispatch_mode mode,
            std::int32_t       priority,
            mcdk_event_handler handler,
            void*              user
        ) noexcept {
            mcdk_handle token = 0;
            guardVoid([&] {
                if (registry().find(self) == nullptr) {
                    return;
                }
                EventId internalId{};
                if (!fromAbiEventId(eventId, internalId)) {
                    setError(MCDK_ERR_INVALID_ARGUMENT, "unknown event id");
                    return;
                }
                if (mode != MCDK_DISPATCH_QUEUED && mode != MCDK_DISPATCH_SYNC && mode != MCDK_DISPATCH_MAIN) {
                    setError(MCDK_ERR_INVALID_ARGUMENT, "unknown dispatch mode");
                    return;
                }
                token = subscribeEvent(self, internalId, mode, priority, handler, user);
            });
            return token;
        }

        void MCDK_CALL eventsUnsubscribe(mcdk_handle self, mcdk_handle token) noexcept {
            guardVoid([&] {
                if (registry().find(self) == nullptr) {
                    return;
                }
                unsubscribeEvent(self, token);
            });
        }

        mcdk_status MCDK_CALL eventsEmit(
            mcdk_handle   self,
            std::uint32_t eventId,
            const void* /*payload*/,
            std::uint32_t /*payloadSize*/
        ) noexcept {
            return guard([&]() -> mcdk_status {
                if (registry().find(self) == nullptr) {
                    return MCDK_ERR_INVALID_HANDLE;
                }
                EventId internalId{};
                if (!fromAbiEventId(eventId, internalId)) {
                    return setError(MCDK_ERR_INVALID_ARGUMENT, "unknown event id");
                }
                // 插件发射内置的 mcdk.* 事件会让宿主状态与事件流不一致，不允许。
                // 插件自定义事件的命名空间待 mcdk.core::register_interface 一并开放，
                return setError(MCDK_ERR_NOT_SUPPORTED, "plugins cannot emit built-in mcdk.* events");
            });
        }

        void MCDK_CALL eventsPostMain(mcdk_handle self, void(MCDK_CALL* fn)(void*), void* user) noexcept {
            guardVoid([&] {
                if (registry().find(self) == nullptr) {
                    return;
                }
                postMainThreadWork(fn, user);
            });
        }

        constexpr mcdk_iface_events kTable = {
            /* struct_size */ static_cast<uint32_t>(sizeof(mcdk_iface_events)),
            /* _reserved   */ 0u,
            /* resolve     */ &eventsResolve,
            /* subscribe   */ &eventsSubscribe,
            /* unsubscribe */ &eventsUnsubscribe,
            /* emit        */ &eventsEmit,
            /* post_main   */ &eventsPostMain,
        };

    } // namespace

    const mcdk_iface_events* eventsTable() noexcept { return &kTable; }

} // namespace mcdk::plugin_host::detail
