#pragma once

#include <string_view>

#include "abi/iface/console.h"
#include "detail/abi_bridge.hpp"

namespace mcdk {

    enum class LogLevel { Trace, Debug, Info, Warn, Error };

    enum class Color { Default, Green, Red, Blue, Yellow, Cyan, Magenta, White, Black, Gray, DarkGray };
    // 线程安全：底下的 ABI 函数可从任意线程调用，宿主负责串行化并保证单次调用
    // 的消息整体原子写出。因此这里不需要任何加锁。
    class Console {
    public:
        Console() = default;

        Console(mcdk_handle self, const mcdk_iface_console* table) noexcept : mSelf(self), mTable(table) {}

        [[nodiscard]] bool available() const noexcept { return mTable != nullptr; }

        void log(LogLevel level, std::string_view message) const noexcept {
            if (!detail::ifaceHas(mTable, &mcdk_iface_console::log)) {
                return;
            }
            mTable->log(mSelf, toAbi(level), detail::toAbi(message));
        }

        void print(Color color, std::string_view message) const noexcept {
            if (!detail::ifaceHas(mTable, &mcdk_iface_console::log_colored)) {
                return;
            }
            mTable->log_colored(mSelf, toAbi(color), detail::toAbi(message));
        }

        void trace(std::string_view message) const noexcept { log(LogLevel::Trace, message); }
        void debug(std::string_view message) const noexcept { log(LogLevel::Debug, message); }
        void info(std::string_view message) const noexcept { log(LogLevel::Info, message); }
        void warn(std::string_view message) const noexcept { log(LogLevel::Warn, message); }
        void error(std::string_view message) const noexcept { log(LogLevel::Error, message); }

    private:
        // 显式 switch 映射，避免 ABI 枚举值与本地 enum class
        // 次序将来可以调整，二者是两个独立的枚举。写成强制转换的话，哪天有人
        [[nodiscard]] static constexpr mcdk_log_level toAbi(LogLevel level) noexcept {
            switch (level) {
            case LogLevel::Trace:
                return MCDK_LOG_TRACE;
            case LogLevel::Debug:
                return MCDK_LOG_DEBUG;
            case LogLevel::Info:
                return MCDK_LOG_INFO;
            case LogLevel::Warn:
                return MCDK_LOG_WARN;
            case LogLevel::Error:
                return MCDK_LOG_ERROR;
            }
            return MCDK_LOG_INFO;
        }

        [[nodiscard]] static constexpr mcdk_color toAbi(Color color) noexcept {
            switch (color) {
            case Color::Default:
                return MCDK_COLOR_DEFAULT;
            case Color::Green:
                return MCDK_COLOR_GREEN;
            case Color::Red:
                return MCDK_COLOR_RED;
            case Color::Blue:
                return MCDK_COLOR_BLUE;
            case Color::Yellow:
                return MCDK_COLOR_YELLOW;
            case Color::Cyan:
                return MCDK_COLOR_CYAN;
            case Color::Magenta:
                return MCDK_COLOR_MAGENTA;
            case Color::White:
                return MCDK_COLOR_WHITE;
            case Color::Black:
                return MCDK_COLOR_BLACK;
            case Color::Gray:
                return MCDK_COLOR_GRAY;
            case Color::DarkGray:
                return MCDK_COLOR_DARK_GRAY;
            }
            return MCDK_COLOR_DEFAULT;
        }

        mcdk_handle               mSelf  = 0;
        const mcdk_iface_console* mTable = nullptr;
    };

} // namespace mcdk
