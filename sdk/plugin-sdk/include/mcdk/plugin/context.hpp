#pragma once

#include <string>
#include <string_view>

#include "abi/entry.h"
#include "console.hpp"
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

        // 由 SDK 的入口胶水调用，插件不应直接使用。
        void bindHost(const mcdk_host_info& host) {
            mSelf        = host.self;
            mHostVersion = detail::toString(host.host_version);
            mConsole     = Console(
                mSelf,
                detail::getInterface<mcdk_iface_console>(host, MCDK_IFACE_CONSOLE_NAME, MCDK_IFACE_CONSOLE_VERSION)
            );
        }

    private:
        mcdk_handle mSelf = 0;
        std::string mHostVersion;
        Console     mConsole;
    };

} // namespace mcdk
