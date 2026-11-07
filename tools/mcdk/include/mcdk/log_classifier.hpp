#pragma once
#include <cstdint>
#include <string_view>

namespace mcdk {

    enum class LogLineKind : std::uint8_t { Developer, Success, Error, Warn, Debug, Plain };

    // 控制台着色用的分类。含 "[INFO][Developer]"（区分大小写）优先；否则按
    // SUC > ERROR > WARN > DEBUG 取第一个出现过的（任意位置，不区分大小写）。
    [[nodiscard]] LogLineKind classifyLogLine(std::string_view line) noexcept;

} // namespace mcdk
