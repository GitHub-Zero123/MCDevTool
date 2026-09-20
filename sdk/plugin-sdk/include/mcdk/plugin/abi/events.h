/*
 * MCDK 插件 ABI —— 事件与 payload
 * 纯 C99。约束见 core.h 顶部说明。
 */
#ifndef MCDK_PLUGIN_ABI_EVENTS_H
#define MCDK_PLUGIN_ABI_EVENTS_H

#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif
/* 派发模式                                                            */
/*
 * 回调跑在哪个线程是 ABI 的一部分，由订阅方在订阅时声明。
 * 各事件的发射线程见 04-events.md §4 的「发射线程」列。
 */
typedef uint32_t mcdk_dispatch_mode;
enum {
    /* 默认。payload 深拷贝后入有界队列，宿主的派发线程串行回调。
       不拖住发射方，代价是看不到返回值——否决无效。
       全部插件共用这一条派发线程，你慢会拖慢别人。 */
    MCDK_DISPATCH_QUEUED = 0,
    /* 发射线程内同步调用，可否决。阻塞它就是阻塞那个子系统：
       mcdk.log.line 上阻塞会卡住游戏日志管道，进而阻塞游戏进程。 */
    MCDK_DISPATCH_SYNC = 1
    /* 2 曾是 MCDK_DISPATCH_MAIN（投递到宿主主线程），v1 未发布前已移除：
       宿主没有任何线程亲和的资源，它解决的是一个不存在的问题。
       该取值永久保留、不得复用，以便将来真需要时按原号加回。 */
};

typedef uint32_t mcdk_event_result;
enum {
    MCDK_EVENT_CONTINUE = 0,
    MCDK_EVENT_STOP     = 1, /* 停止后续处理器 */
    /* 仅对可否决事件有效；异常按 CONTINUE 处理。 */
    MCDK_EVENT_VETO = 2
};
/* 事件封包                                                            */
typedef struct mcdk_event {
    uint32_t struct_size;
    uint32_t event_id;
    uint32_t payload_version;
    uint32_t payload_size;
    /* 指向某个 mcdk_ev_* 结构体；QUEUED 模式会深拷贝，回调返回后指针失效。 */
    const void* payload;
} mcdk_event;

typedef mcdk_event_result(MCDK_CALL* mcdk_event_handler)(const mcdk_event* event, void* user);
/* v1 事件的 payload                                                   */
/*
 * 一律是版本化 POD，不用 JSON：日志事件每秒数百条，序列化开销不可接受。
 * 只在实现时才定义——按 02 §5.9，一旦定义就只能追加不能改，过早固化没有
 */

/* mcdk.mcp.register.before / .finish 共用 */
typedef struct mcdk_ev_mcp_register {
    uint32_t struct_size;
    uint32_t tool_count; /* before: 已有内置工具数；finish: 最终总数 */
} mcdk_ev_mcp_register;

/* mcdk.game.launch.before —— 可否决 */
typedef struct mcdk_ev_game_launch_before {
    uint32_t struct_size;
    uint32_t _reserved;
    mcdk_str exe_path;        /* 借用 */
    mcdk_str dev_config_path; /* 借用；未启用自动进入存档时 len == 0 */
    /* 后续版本在此追加 env_builder 句柄（04-events.md §4.1） */
} mcdk_ev_game_launch_before;

/* mcdk.game.launch.finish */
typedef struct mcdk_ev_game_launch_finish {
    uint32_t struct_size;
    uint32_t pid;
    mcdk_str exe_path; /* 借用 */
} mcdk_ev_game_launch_finish;

/* mcdk.game.exit */
typedef struct mcdk_ev_game_exit {
    uint32_t struct_size;
    uint32_t pid;
    int32_t  exit_code;
    uint32_t _reserved;
} mcdk_ev_game_exit;

/* mcdk.log.line / .error 共用 —— 高频，默认 QUEUED */
typedef struct mcdk_ev_log_line {
    uint32_t struct_size;
    uint32_t channel; /* 0 = stdout, 1 = stderr */
    int64_t  timestamp_ms;
    mcdk_str text; /* 借用，回调返回即失效 */
} mcdk_ev_log_line;

/* mcdk.ipc.client.connected / .disconnected 共用 */
typedef struct mcdk_ev_ipc_client {
    uint32_t struct_size;
    uint32_t client_count; /* 本次变化后的调试 IPC 客户端数 */
} mcdk_ev_ipc_client;
/* 事件名（resolve 的输入）                                             */
/*
 * 数值 id 只在本进程本次运行内稳定，禁止序列化或硬编码；名字才是稳定契约。
 * resolve 返回 0 表示该宿主不认识这个名字，插件应据此优雅降级。
 */
#define MCDK_EVENT_MCP_REGISTER_BEFORE "mcdk.mcp.register.before"
#define MCDK_EVENT_MCP_REGISTER_FINISH "mcdk.mcp.register.finish"
#define MCDK_EVENT_GAME_LAUNCH_BEFORE "mcdk.game.launch.before"
#define MCDK_EVENT_GAME_LAUNCH_FINISH "mcdk.game.launch.finish"
#define MCDK_EVENT_GAME_EXIT "mcdk.game.exit"
#define MCDK_EVENT_LOG_LINE "mcdk.log.line"
#define MCDK_EVENT_LOG_ERROR "mcdk.log.error"
#define MCDK_EVENT_IPC_CLIENT_CONNECTED "mcdk.ipc.client.connected"
#define MCDK_EVENT_IPC_CLIENT_DISCONNECTED "mcdk.ipc.client.disconnected"

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MCDK_PLUGIN_ABI_EVENTS_H */
