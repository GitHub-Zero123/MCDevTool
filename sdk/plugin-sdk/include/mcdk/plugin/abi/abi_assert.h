/*
 * MCDK 插件 ABI —— 布局与类型的机械防线
 *
 * 本文件是 abi/ 下唯一的 C++ 头。宿主与 SDK 各自在恰好一个翻译单元中包含它，
 * 之后任何字段类型变更、顺序调整、意外填充、或者把 C++ 类型塞进 ABI 结构体的
 * 行为，都会让两侧同时编译失败。
 *
 * 这是把 02-abi-contract.md 的纪律变成机器判定的一半；另一半是 CI 对上一版
 * 头文件做的字段比对（09-compatibility.md §4）。文档能被忽略，编译错误不能。
 *
 * 其中 is_trivially_copyable 一条最为关键：它是防止 std::string、
 * std::function、std::vector 这类布局随标准库和 _ITERATOR_DEBUG_LEVEL 变化的
 * 类型混入边界的直接闸门。这类类型跨 DLL 传递时不会报错，只会静默损坏内存。
 */
#ifndef MCDK_PLUGIN_ABI_ASSERT_H
#define MCDK_PLUGIN_ABI_ASSERT_H

#ifndef __cplusplus
#error "abi_assert.h is C++ only; every other header under abi/ must stay pure C99."
#endif

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "core.h"
#include "entry.h"
#include "iface/console.h"

namespace mcdk::abi_assert {

/* 每个跨边界的结构体都必须通过的三项检查。 */
#define MCDK_ABI_CHECK_LAYOUT(T)                                                                                       \
    static_assert(std::is_standard_layout_v<T>, #T " must be standard layout to cross the ABI");                       \
    static_assert(                                                                                                     \
        std::is_trivially_copyable_v<T>,                                                                               \
        #T " must be trivially copyable — a C++ type (std::string / std::function / std::vector / ...) has leaked "    \
           "into the ABI. See docs/plugin-system/02-abi-contract.md §3.3."                                             \
    );                                                                                                                 \
    static_assert(std::is_trivially_destructible_v<T>, #T " must be trivially destructible")

/* 首字段必须是 struct_size，否则追加式演进的探测机制无从谈起。 */
#define MCDK_ABI_CHECK_GROWABLE(T)                                                                                     \
    MCDK_ABI_CHECK_LAYOUT(T);                                                                                          \
    static_assert(offsetof(T, struct_size) == 0, #T " must begin with uint32_t struct_size");                          \
    static_assert(sizeof(decltype(T::struct_size)) == 4, #T "::struct_size must be exactly uint32_t")

    /* -------------------------------------------------------------- */
    /* 基础标量                                                        */
    /* -------------------------------------------------------------- */
    static_assert(sizeof(mcdk_bool) == 1, "mcdk_bool must be exactly one byte");
    static_assert(sizeof(mcdk_status) == 4, "mcdk_status must be exactly four bytes");
    static_assert(sizeof(mcdk_handle) == 8, "mcdk_handle must be exactly eight bytes");
    static_assert(std::is_unsigned_v<mcdk_bool>, "mcdk_bool must be unsigned");
    static_assert(std::is_signed_v<mcdk_status>, "mcdk_status must be signed so negative error codes work");

    /*
     * size_t 与指针同宽在本项目的全部目标平台上成立，mcdk_str 的布局依赖它。
     * 操作系统加载器已经保证宿主与插件指针宽度相同，这里只是把该前提显式化，
     * 万一移植到不满足的平台会立刻失败而不是悄悄错位。
     */
    static_assert(sizeof(size_t) == sizeof(void*), "size_t must be pointer-sized");

    /* -------------------------------------------------------------- */
    /* mcdk_str：边界上唯一的字符串形态                                  */
    /* -------------------------------------------------------------- */
    MCDK_ABI_CHECK_LAYOUT(mcdk_str);
    static_assert(sizeof(mcdk_str) == 2 * sizeof(void*), "mcdk_str must be exactly two pointer-sized fields");
    static_assert(alignof(mcdk_str) == alignof(void*), "mcdk_str must be pointer-aligned");
    static_assert(offsetof(mcdk_str, ptr) == 0, "mcdk_str::ptr must come first");
    static_assert(offsetof(mcdk_str, len) == sizeof(void*), "mcdk_str::len must follow ptr with no padding");

    /* -------------------------------------------------------------- */
    /* 入口结构体                                                      */
    /* -------------------------------------------------------------- */
    MCDK_ABI_CHECK_GROWABLE(mcdk_host_info);
    static_assert(offsetof(mcdk_host_info, abi_major) == 4, "mcdk_host_info field order changed");
    static_assert(offsetof(mcdk_host_info, abi_minor) == 8, "mcdk_host_info field order changed");
    static_assert(offsetof(mcdk_host_info, host_version) == 16, "mcdk_host_info field order changed");

    MCDK_ABI_CHECK_GROWABLE(mcdk_plugin_desc);
    static_assert(offsetof(mcdk_plugin_desc, abi_major) == 4, "mcdk_plugin_desc field order changed");
    static_assert(offsetof(mcdk_plugin_desc, abi_minor) == 8, "mcdk_plugin_desc field order changed");
    static_assert(offsetof(mcdk_plugin_desc, min_stage) == 12, "mcdk_plugin_desc field order changed");
    static_assert(offsetof(mcdk_plugin_desc, id) == 16, "mcdk_plugin_desc field order changed");

    /* 函数指针也必须是平凡类型，捕获式 lambda 不可能意外混入。 */
    static_assert(std::is_pointer_v<mcdk_get_interface_fn>, "mcdk_get_interface_fn must be a plain function pointer");
    static_assert(std::is_pointer_v<mcdk_plugin_entry_fn>, "mcdk_plugin_entry_fn must be a plain function pointer");

    /* -------------------------------------------------------------- */
    /* 接口表                                                          */
    /* -------------------------------------------------------------- */
    MCDK_ABI_CHECK_GROWABLE(mcdk_iface_console);
    static_assert(
        offsetof(mcdk_iface_console, log) == 8,
        "mcdk_iface_console::log must sit right after struct_size and its padding"
    );

    /* -------------------------------------------------------------- */
    /* 枚举底层类型                                                     */
    /* -------------------------------------------------------------- */
    /* 必须是定宽整数别名而非 typedef enum——后者的底层类型由编译器自行决定。 */
    static_assert(std::is_same_v<mcdk_stage, uint32_t>, "mcdk_stage must be a fixed-width alias");
    static_assert(std::is_same_v<mcdk_log_level, uint32_t>, "mcdk_log_level must be a fixed-width alias");
    static_assert(std::is_same_v<mcdk_color, uint32_t>, "mcdk_color must be a fixed-width alias");

#undef MCDK_ABI_CHECK_GROWABLE
#undef MCDK_ABI_CHECK_LAYOUT

} // namespace mcdk::abi_assert

#endif /* MCDK_PLUGIN_ABI_ASSERT_H */
