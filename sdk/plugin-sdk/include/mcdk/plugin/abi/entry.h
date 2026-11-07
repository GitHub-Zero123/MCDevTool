/*
 * MCDK 插件 ABI —— 入口点与接口查询
 * 纯 C99。约束见 core.h 顶部说明。
 */
#ifndef MCDK_PLUGIN_ABI_ENTRY_H
#define MCDK_PLUGIN_ABI_ENTRY_H

#include "core.h"

#ifdef __cplusplus
extern "C" {
#endif
/* 接口查询                                                            */
/*
 * 返回指向常量接口表的指针，生命周期与宿主进程相同；名字未知或版本不匹配
 * 时返回 NULL，插件应据此优雅降级而非判定加载失败。
 */
typedef const void*(MCDK_CALL* mcdk_get_interface_fn)(const char* name, uint32_t version);
/* 宿主交给插件的信息                                                    */
typedef struct mcdk_host_info {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t _reserved;

    mcdk_str    host_version; /* 例如 "1.4.2"，借用 */
    mcdk_handle self;         /* 本插件实例句柄，此后每次调用都要带上 */

    mcdk_get_interface_fn get_interface;
} mcdk_host_info;
/* 插件回填给宿主的描述                                                  */
/*
 * 阶段回调与卸载回调必须是 noexcept 的 C 蹦床，插件侧异常由 SDK
 * 屏障就地吃掉并转成状态码，绝不允许穿越本边界（02-abi-contract.md §4）。
 */
typedef struct mcdk_plugin_desc {
    uint32_t struct_size;
    uint32_t abi_major; /* 由 SDK 在编译期写死，宿主校验 */
    uint32_t abi_minor;
    uint32_t min_stage; /* 最低所需阶段，取值见 mcdk_stage */

    mcdk_str id;      /* 反向域名，例如 "com.example.my-plugin" */
    mcdk_str name;    /* 展示名 */
    mcdk_str version; /* 插件自身版本 */

    void* user; /* 原样回传给下面两个回调 */

    /*
     * 返回 MCDK_OK 表示该阶段处理成功。
     * 返回值不可省略：插件侧的异常被 SDK 屏障就地吃掉后，宿主没有别的途径知道
    */
    mcdk_status(MCDK_CALL* on_stage)(void* user, mcdk_stage stage);

    /*
     * 无返回值：卸载失败没有宿主可采取的补救动作，按约定记录后继续
     * （02-abi-contract.md §4.2）。
    */
    void(MCDK_CALL* on_unload)(void* user);
} mcdk_plugin_desc;
/* 入口符号                                                            */
/*
 * 插件导出唯一一个符合本签名的函数，默认名为 MCDK_PLUGIN_ENTRY_SYMBOL，
 * 可在 plugin.json 的 entry_symbol 字段中改名（同一动态库承载多个插件时需要）。
 */
typedef mcdk_bool(MCDK_CALL* mcdk_plugin_entry_fn)(const mcdk_host_info* host, mcdk_plugin_desc* out_desc);

#define MCDK_PLUGIN_ENTRY_SYMBOL "mcdk_plugin_entry"

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MCDK_PLUGIN_ABI_ENTRY_H */
