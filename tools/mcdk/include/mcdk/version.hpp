#pragma once

//
// mcdk 的版本号。
//
// 仓库有两套构建系统（CMake 与 xmake），把版本塞进任一方的 configure_file 都会让
// 另一方拿不到。所以这里就是唯一真源：一个普通头文件常量。
//
// **发版时必须与 git tag 同步修改。** 目前没有自动校验，因为没有能同时约束两套
// 构建系统的低成本做法；真要防漏，该在发版脚本里比对 `git describe` 与本常量。
//

#include <string_view>

namespace mcdk {

    inline constexpr std::string_view kVersion = "1.6.1";

} // namespace mcdk
