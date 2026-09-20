/*
 * MCDK 插件 ABI —— mcdk.info/1
 * 纯 C99。约束见 ../core.h 顶部说明。
 */
#ifndef MCDK_PLUGIN_ABI_IFACE_INFO_H
#define MCDK_PLUGIN_ABI_IFACE_INFO_H

#include "../core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MCDK_IFACE_INFO_NAME    "mcdk.info"
#define MCDK_IFACE_INFO_VERSION 1u

/*
 * 一次会话的信息快照。
 * 标量字段是调用时刻的快照；字符串字段是借用，get_session 返回后即失效，
 */
typedef struct mcdk_session_info {
    uint32_t  struct_size;
    uint32_t  mcdk_pid;
    uint32_t  game_pid;      /* 0 = 游戏进程尚未创建 */
    uint16_t  game_ipc_port; /* 0 = 调试 IPC 未启用 */
    uint16_t  mcp_port;      /* 0 = MCP 未启用 */
    mcdk_bool mcp_enabled;
    mcdk_bool game_debug_ready; /* 调试 IPC 已有客户端，即游戏已进入世界 */
    /* 显式填充。mcdk_bool 是 uint8_t，到这里才 18 字节，而下方 mcdk_str 要 8 字节对齐。
       隐式填充在 ABI 上是不允许的（02-abi-contract.md §5.3）：它使布局依赖于编译器行为，
       也让将来在这里加字段变成一次静默的布局变更。 */
    uint8_t _reserved[6];
    mcdk_str mcp_ip;
    mcdk_str  game_exe_path;
    mcdk_str  project_root;
    mcdk_str  world_name;
    mcdk_str  world_folder_name;
    mcdk_str  world_runtime_path;
    mcdk_str  world_source_path; /* len == 0 表示非玩法地图工程 */
} mcdk_session_info;

typedef struct mcdk_iface_info {
    uint32_t struct_size;
    uint32_t _reserved;

    /*
     * 回填会话快照。out_info->struct_size 必须由调用方先置为 sizeof，
     * 宿主据此判断对方认识到哪个字段为止，只写它认识的部分。
    */
    mcdk_status(MCDK_CALL* get_session)(mcdk_handle self, mcdk_session_info* out_info);
} mcdk_iface_info;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MCDK_PLUGIN_ABI_IFACE_INFO_H */
