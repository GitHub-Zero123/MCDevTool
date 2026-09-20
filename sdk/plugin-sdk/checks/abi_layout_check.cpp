/*
 * 本文件没有运行期用途：包含 abi_assert.h 即完成全部编译期校验。
 * 宿主侧在 components/plugin-host 中同样包含一次，两边都必须通过。
 */
#include <mcdk/plugin/abi/abi_assert.h>

/* 提供一个外部符号，避免部分工具链对空静态库告警。 */
int mcdkAbiLayoutCheck() { return 0; }
