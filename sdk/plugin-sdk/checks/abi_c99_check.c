/*
 * 本文件没有运行期用途：它存在的唯一理由是证明 abi/ 下的头能被 C 编译器
 * 单独编译通过。
 *
 * 一旦有人往 ABI 头里塞进任何 C++ 构造——最常见的是 std::string、
 * std::function 这类布局随标准库实现变化的类型——这个目标会立刻编译失败。
 * 配套的布局断言见 abi_layout_check.cpp。
 */
#include <mcdk/plugin/abi/core.h>
#include <mcdk/plugin/abi/entry.h>
#include <mcdk/plugin/abi/iface/console.h>

/* 顺带验证接口表可以在纯 C 下正常定义与填充。 */
static void MCDK_CALL mcdkAbiCheckLog(mcdk_handle self, mcdk_log_level level, mcdk_str message) {
    (void)self;
    (void)level;
    (void)message;
}

static const mcdk_iface_console kAbiCheckConsoleTable = {
    /* struct_size */ (uint32_t)sizeof(mcdk_iface_console),
    /* _reserved   */ 0u,
    /* log         */ mcdkAbiCheckLog,
    /* log_colored */ NULL
};

/* 提供一个外部符号，避免部分工具链对空静态库告警。 */
int mcdkAbiC99Check(void) {
    mcdk_str empty;
    empty.ptr = NULL;
    empty.len = 0u;

    if (kAbiCheckConsoleTable.log != NULL) {
        kAbiCheckConsoleTable.log((mcdk_handle)0, MCDK_LOG_INFO, empty);
    }
    return (int)MCDK_OK;
}
