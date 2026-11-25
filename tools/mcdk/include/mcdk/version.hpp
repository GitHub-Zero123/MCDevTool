#pragma once

//
// mcdk 的版本号。
//
// 仓库有两套构建系统（CMake 与 xmake），把版本塞进任一方的 configure_file 都会让
// 另一方拿不到。所以这里就是唯一真源：一个普通头文件常量。
//
// 规范：
// - 格式为 MAJOR.MINOR.PATCH，三段十进制数字，不带 "v" 前缀，也不带预发布或构建后缀。
//   下面的 static_assert 在编译期校验。
// - 发版 tag 是 "v" 加本常量，例如 v1.6.5。
// - **发版时必须与 git tag 同步修改。** CMake 配置时会比对最近的 tag
//   （tools/mcdk/CMakeLists.txt），常量落后时给出警告；xmake 没有这项检查。
//

#include <cstddef>
#include <string_view>

namespace mcdk {

    inline constexpr std::string_view kVersion = "1.6.5";

    namespace detail {
        // 恰好三段、每段是不带前导零的十进制数。
        constexpr bool isReleaseVersion(std::string_view text) {
            int         parts = 0;
            std::size_t index = 0;
            while (true) {
                const std::size_t start = index;
                while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
                    ++index;
                }
                if (index == start || (index - start > 1 && text[start] == '0')) {
                    return false;
                }
                ++parts;
                if (index == text.size()) {
                    return parts == 3;
                }
                if (text[index] != '.') {
                    return false;
                }
                ++index;
            }
        }
    } // namespace detail

    static_assert(detail::isReleaseVersion(kVersion), "kVersion 必须是 MAJOR.MINOR.PATCH，例如 1.6.5");

} // namespace mcdk
