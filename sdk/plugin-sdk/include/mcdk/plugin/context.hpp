#pragma once

#include <string>
#include <string_view>

#include "abi/entry.h"
#include "abi/iface/core.h"
#include "abi/iface/game.h"
#include "abi/iface/info.h"
#include "abi/iface/log.h"
#include "abi/iface/mcp.h"
#include "console.hpp"
#include "events.hpp"
#include "game.hpp"
#include "info.hpp"
#include "log.hpp"
#include "mcp.hpp"
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

        // 会话信息：游戏路径、MCP 端口、游戏 IPC 端口等。
        [[nodiscard]] const Info& info() const noexcept { return mInfo; }

        // 游戏日志缓冲区。
        [[nodiscard]] const Log& log() const noexcept { return mLog; }

        // 游戏进程交互：Python 执行与窗口截图。
        [[nodiscard]] const Game& game() const noexcept { return mGame; }

        // MCP 工具注册。非 const：注册要把 handler 闭包存进来。
        [[nodiscard]] Mcp& mcp() noexcept { return mMcp; }
// .mcdev.json 中本条插件声明的 config 字段，UTF-8 JSON 文本。
// 同一个插件二进制可以声明多次、各带不同 config，据此表现出不同行为。
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

            mInfo = Info(
                mSelf,
                detail::getInterface<mcdk_iface_info>(host, MCDK_IFACE_INFO_NAME, MCDK_IFACE_INFO_VERSION)
            );

            mLog = Log(
                mSelf,
                detail::getInterface<mcdk_iface_log>(host, MCDK_IFACE_LOG_NAME, MCDK_IFACE_LOG_VERSION)
            );

            mGame = Game(
                mSelf,
                detail::getInterface<mcdk_iface_game>(host, MCDK_IFACE_GAME_NAME, MCDK_IFACE_GAME_VERSION)
            );

            mMcp = Mcp(
                mSelf,
                detail::getInterface<mcdk_iface_mcp>(host, MCDK_IFACE_MCP_NAME, MCDK_IFACE_MCP_VERSION)
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
        Info                   mInfo;
        Log                    mLog;
        Game                   mGame;
        Mcp                    mMcp;
        const mcdk_iface_core* mCore = nullptr;
    };

} // namespace mcdk
