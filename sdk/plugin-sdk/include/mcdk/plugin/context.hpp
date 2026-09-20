#pragma once

#include <string>
#include <string_view>

#include "abi/entry.h"
#include "abi/iface/core.h"
#include "console.hpp"
#include "events.hpp"
#include "detail/abi_bridge.hpp"

namespace mcdk {

    // 插件访问宿主能力的唯一入口。由 SDK 在入口函数中构造，随各生命周期回调
    // 传给插件。
    class Context {
    public:
        Context() = default;

        [[nodiscard]] mcdk_handle self() const noexcept { return mSelf; }

        // 宿主版本。注意这是深拷贝后的副本：ABI 给的 mcdk_str 是借用的，
        // 出了入口函数就失效，所以这里必须持有自己的 std::string。
        [[nodiscard]] std::string_view hostVersion() const noexcept { return mHostVersion; }

        [[nodiscard]] const Console& console() const noexcept { return mConsole; }

        // 事件订阅。非 const：订阅要把闭包存进来。
        [[nodiscard]] Events& events() noexcept { return mEvents; }

        // .mcdev.json 中本条插件声明的 config 字段，UTF-8 JSON 文本。
        // 同一个插件二进制可以声明多次、各带不同 config，据此表现出不同行为。
        // 未设置时是字面量 "null"，可以直接丢给任意 JSON 解析器，无需特判。
        //
        // 这里同样是深拷贝：ABI 给的 mcdk_str 是借用的。
        [[nodiscard]] std::string_view configJson() const noexcept { return mConfigJson; }

        // 由 SDK 的入口胶水调用，插件不应直接使用。
        void bindHost(const mcdk_host_info& host) {
            mSelf        = host.self;
            mHostVersion = detail::toString(host.host_version);
            mConsole     = Console(
                mSelf,
                detail::getInterface<mcdk_iface_console>(host, MCDK_IFACE_CONSOLE_NAME, MCDK_IFACE_CONSOLE_VERSION)
            );

            mEvents = Events(
                mSelf,
                detail::getInterface<mcdk_iface_events>(host, MCDK_IFACE_EVENTS_NAME, MCDK_IFACE_EVENTS_VERSION)
            );

            mCore = detail::getInterface<mcdk_iface_core>(host, MCDK_IFACE_CORE_NAME, MCDK_IFACE_CORE_VERSION);
            if (detail::ifaceHas(mCore, &mcdk_iface_core::get_config)) {
                mcdk_str raw{};
                mCore->get_config(mSelf, &raw);
                mConfigJson = detail::toString(raw);
            }
            if (mConfigJson.empty()) {
                mConfigJson = "null";
            }
        }

    private:
        mcdk_handle            mSelf = 0;
        std::string            mHostVersion;
        std::string            mConfigJson = "null";
        Console                mConsole;
        Events                 mEvents;
        const mcdk_iface_core* mCore = nullptr;
    };

} // namespace mcdk
