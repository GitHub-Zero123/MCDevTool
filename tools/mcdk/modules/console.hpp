#pragma once
#include <string>

namespace mcdk {
    enum class ConsoleColor {
        Default,
        Green,   // 绿色（Green）
        Red,     // 红色（Red）
        Blue,    // 蓝色（Blue）
        Yellow,  // 黄色（Yellow）
        Cyan,    // 青色（Cyan）
        Magenta, // 品红（Magenta）
        White,   // 白色（White）
        Black,   // 黑色（Black）
        Gray,    // 亮灰（Light Gray）
        DarkGray // 深灰（Dark Gray）
    };

    // 控制台输出回调类型（由平台相关代码提供实现）
    using ConsoleOutputCallback = std::function<void(const std::string& msg, mcdk::ConsoleColor colorCode)>;
} // namespace mcdk