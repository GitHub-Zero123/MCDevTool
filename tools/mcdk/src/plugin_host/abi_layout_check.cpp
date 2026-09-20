// 宿主侧的 ABI 布局校验。没有运行期用途：包含 abi_assert.h 即完成全部编译期检查。
// SDK 侧在 sdk/plugin-sdk/checks/abi_layout_check.cpp 中同样包含一次。两边都得
#include <mcdk/plugin/abi/abi_assert.h>
