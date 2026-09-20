/*
 * MCDK 插件 ABI —— mcdk.log/1
 *
 * 纯 C99。约束见 ../core.h 顶部说明。
 */
#ifndef MCDK_PLUGIN_ABI_IFACE_LOG_H
#define MCDK_PLUGIN_ABI_IFACE_LOG_H

#include "../core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MCDK_IFACE_LOG_NAME    "mcdk.log"
#define MCDK_IFACE_LOG_VERSION 1u

typedef uint32_t mcdk_log_channel;
enum {
    MCDK_LOG_CHANNEL_STDOUT = 0,
    MCDK_LOG_CHANNEL_STDERR = 1
};

typedef uint32_t mcdk_log_order;
enum {
    MCDK_LOG_ORDER_DESC = 0, /* 由新到旧 */
    MCDK_LOG_ORDER_ASC  = 1  /* 由旧到新 */
};

/*
 * 索引语义与现有 MCP 工具 get_latest_logs / get_log_range 完全一致：
 * 索引 0 是最新一条，1 是次新，以此类推。
 */
typedef struct mcdk_log_query {
    uint32_t         struct_size;
    mcdk_log_channel channel;
    uint32_t         start_index; /* 含 */
    uint32_t         end_index;   /* 不含；等于 start_index 时改用 max_count 截断 */
    uint32_t         max_count;   /* 0 = 不限，仍受缓冲区容量约束 */
    mcdk_log_order   order;
} mcdk_log_query;

typedef struct mcdk_log_entry {
    uint32_t struct_size;
    uint32_t index; /* 相对最新条目的索引 */
    int64_t  timestamp_ms;
    mcdk_str text; /* 借用，sink 返回即失效 */
} mcdk_log_entry;

/*
 * 宿主在持有 LogBuffer 锁的状态下逐条调用 sink。因此：
 *   - sink 必须极短，只做拷贝或匹配；
 *   - sink 内禁止调用任何其他 mcdk 接口——包括 mcdk.console，会死锁；
 *   - sink 内禁止阻塞、等待其他线程。
 * 需要复杂处理时，先在 sink 里把文本拷进插件自己的容器，query 返回后再处理。
 * SDK 的 ctx.log().query(...) 默认就是这么做的，用户拿不到裸 sink。
 */
typedef void(MCDK_CALL* mcdk_log_sink)(void* user, const mcdk_log_entry* entry);

typedef struct mcdk_iface_log {
    uint32_t struct_size;
    uint32_t _reserved;

    mcdk_status(MCDK_CALL* query)(mcdk_handle self, const mcdk_log_query* query, mcdk_log_sink sink, void* user);

    /* 指定通道当前的条目数。 */
    uint32_t(MCDK_CALL* count)(mcdk_handle self, mcdk_log_channel channel);
} mcdk_iface_log;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MCDK_PLUGIN_ABI_IFACE_LOG_H */
