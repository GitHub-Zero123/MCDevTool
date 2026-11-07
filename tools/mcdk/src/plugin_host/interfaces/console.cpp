// mcdk.console/1 的宿主实现。
// 本文件里的每个导出函数都必须经过 guardVoid / guard，没有例外
#include <string>

#include <mcdk/plugin/abi/iface/console.h>
#include <mcdk/plugin_host/guard.hpp>

#include "../registry.hpp"

namespace mcdk::plugin_host::detail {

    namespace {

        [[nodiscard]] std::string toString(mcdk_str text) {
            if (text.ptr == nullptr || text.len == 0) {
                return {};
            }
            return std::string(text.ptr, text.len);
        }
        // 显式 switch 而非 static_cast：ABI 枚举值永久冻结，ConsoleColor 是宿主
        // 内部枚举、可以自由调整。写成强制转换的话，哪天有人往 ConsoleColor
        [[nodiscard]] ConsoleColor toConsoleColor(mcdk_color color) noexcept {
            switch (color) {
            case MCDK_COLOR_GREEN:
                return ConsoleColor::Green;
            case MCDK_COLOR_RED:
                return ConsoleColor::Red;
            case MCDK_COLOR_BLUE:
                return ConsoleColor::Blue;
            case MCDK_COLOR_YELLOW:
                return ConsoleColor::Yellow;
            case MCDK_COLOR_CYAN:
                return ConsoleColor::Cyan;
            case MCDK_COLOR_MAGENTA:
                return ConsoleColor::Magenta;
            case MCDK_COLOR_WHITE:
                return ConsoleColor::White;
            case MCDK_COLOR_BLACK:
                return ConsoleColor::Black;
            case MCDK_COLOR_GRAY:
                return ConsoleColor::Gray;
            case MCDK_COLOR_DARK_GRAY:
                return ConsoleColor::DarkGray;
            case MCDK_COLOR_DEFAULT:
            default:
                return ConsoleColor::Default;
            }
        }

        [[nodiscard]] ConsoleColor colorForLevel(mcdk_log_level level) noexcept {
            switch (level) {
            case MCDK_LOG_TRACE:
            case MCDK_LOG_DEBUG:
                return ConsoleColor::DarkGray;
            case MCDK_LOG_WARN:
                return ConsoleColor::Yellow;
            case MCDK_LOG_ERROR:
                return ConsoleColor::Red;
            case MCDK_LOG_INFO:
            default:
                return ConsoleColor::Default;
            }
        }

        // 输出统一带上插件 id，使用户一眼能看出哪条日志是谁打的。
        void emit(mcdk_handle self, ConsoleColor color, mcdk_str message) {
            const auto* record = registry().find(self);
            if (record == nullptr) {
                // 句柄已作废（插件的线程没 join 干净）。静默丢弃，绝不崩溃。
                return;
            }
            const auto& output = outputCallback();
            if (!output) {
                return;
            }
            output("[" + record->id + "] " + toString(message), color);
        }

        void MCDK_CALL consoleLog(mcdk_handle self, mcdk_log_level level, mcdk_str message) noexcept {
            guardVoid([&] { emit(self, colorForLevel(level), message); });
        }

        void MCDK_CALL consoleLogColored(mcdk_handle self, mcdk_color color, mcdk_str message) noexcept {
            guardVoid([&] { emit(self, toConsoleColor(color), message); });
        }

        constexpr mcdk_iface_console kTable = {
            /* struct_size */ static_cast<uint32_t>(sizeof(mcdk_iface_console)),
            /* _reserved   */ 0u,
            /* log         */ &consoleLog,
            /* log_colored */ &consoleLogColored,
        };

    } // namespace

    const mcdk_iface_console* consoleTable() noexcept { return &kTable; }

} // namespace mcdk::plugin_host::detail
