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
 * 宿主先在 LogBuffer 锁内取快照，放锁，再逐条调用 sink——**sink 跑在锁外**。
 * 因此约束只有一条：entry->text 是借用的，sink 返回后即失效，要留必须拷走。
 * 在 sink 里调 mcdk.console、再调一次 query、甚至阻塞一会儿都是允许的。
 *
 * 早期版本曾持锁回调（为了省掉拷贝），但那会让一个慢 sink 卡住日志摄入，
 * 进而填满游戏的 stdout 管道、把游戏进程阻塞在 write 上。不值得。
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
