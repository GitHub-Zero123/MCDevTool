/*
 * MCDK 插件 ABI —— mcdk.core/1
 * 纯 C99。约束见 ../core.h 顶部说明。
 */
#ifndef MCDK_PLUGIN_ABI_IFACE_CORE_H
#define MCDK_PLUGIN_ABI_IFACE_CORE_H

#include "../core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MCDK_IFACE_CORE_NAME    "mcdk.core"
#define MCDK_IFACE_CORE_VERSION 1u

typedef struct mcdk_iface_core {
    uint32_t struct_size;
    uint32_t _reserved;

    /*
     * 取出宿主侧线程局部错误槽中的消息。
     * 借用：指向宿主的线程局部缓冲，仅在下一次同线程 ABI 调用前有效，
    */
    void(MCDK_CALL* get_last_error)(mcdk_handle self, mcdk_str* out_message);

    /* 宿主版本串，例如 "1.4.2"。借用。 */
    void(MCDK_CALL* get_host_version)(mcdk_handle self, mcdk_str* out_version);

    /* 当前生命周期阶段，取值见 mcdk_stage。 */
    mcdk_stage(MCDK_CALL* get_stage)(mcdk_handle self);

    /*
     * 取回 .mcdev.json 中该条插件声明的 config 字段，UTF-8 JSON 文本。借用。
     * 这是「可传参式插件」的入口：同一个插件二进制可以在 plugins 数组里声明
    */
    void(MCDK_CALL* get_config)(mcdk_handle self, mcdk_str* out_config_json);
} mcdk_iface_core;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MCDK_PLUGIN_ABI_IFACE_CORE_H */
