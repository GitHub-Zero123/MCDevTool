/*
 * MCDK 插件 ABI —— mcdk.console/1
 * 纯 C99。约束见 ../core.h 顶部说明。
 */
#ifndef MCDK_PLUGIN_ABI_IFACE_CONSOLE_H
#define MCDK_PLUGIN_ABI_IFACE_CONSOLE_H

#include "../core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MCDK_IFACE_CONSOLE_NAME    "mcdk.console"
#define MCDK_IFACE_CONSOLE_VERSION 1u

typedef uint32_t mcdk_log_level;
enum { MCDK_LOG_TRACE = 0, MCDK_LOG_DEBUG = 1, MCDK_LOG_INFO = 2, MCDK_LOG_WARN = 3, MCDK_LOG_ERROR = 4 };

/*
 * 取值顺序与宿主内部的 mcdk::ConsoleColor 一致，但二者是独立枚举。
 * 本枚举的数值永久冻结，宿主内部枚举可以自由调整。shim 因此禁止写成
 */
typedef uint32_t mcdk_color;
enum {
    MCDK_COLOR_DEFAULT   = 0,
    MCDK_COLOR_GREEN     = 1,
    MCDK_COLOR_RED       = 2,
    MCDK_COLOR_BLUE      = 3,
    MCDK_COLOR_YELLOW    = 4,
    MCDK_COLOR_CYAN      = 5,
    MCDK_COLOR_MAGENTA   = 6,
    MCDK_COLOR_WHITE     = 7,
    MCDK_COLOR_BLACK     = 8,
    MCDK_COLOR_GRAY      = 9,
    MCDK_COLOR_DARK_GRAY = 10
};

/*
 * 两个函数都可从任意线程调用，宿主负责串行化，并保证单次调用的消息整体原子
 * 写出，不与其他线程的输出交错；message 含换行时仍作为一个整体写出。
 */
typedef struct mcdk_iface_console {
    uint32_t struct_size;
    uint32_t _reserved;

    void(MCDK_CALL* log)(mcdk_handle self, mcdk_log_level level, mcdk_str message);
    void(MCDK_CALL* log_colored)(mcdk_handle self, mcdk_color color, mcdk_str message);
} mcdk_iface_console;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MCDK_PLUGIN_ABI_IFACE_CONSOLE_H */
