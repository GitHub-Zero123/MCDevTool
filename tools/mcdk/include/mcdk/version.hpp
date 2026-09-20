#pragma once
// mcdk 的版本号。
// 仓库有两套构建系统（CMake 与 xmake），把版本塞进任一方的 configure_file 都会让
#include <string_view>

namespace mcdk {

    inline constexpr std::string_view kVersion = "1.6.1";

} // namespace mcdk
