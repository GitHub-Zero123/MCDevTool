#pragma once

#include <string>
#include <string_view>

namespace mcdk {

    void               stringReplace(std::string& value, const std::string& from, const std::string& to);
    [[nodiscard]] bool containsIgnoreCase(std::string_view text, std::string_view pattern);

    // 当前 UTC 时间的 ISO-8601 毫秒精度文本，例如 2026-08-29T07:15:03.421Z
    [[nodiscard]] std::string utcTimestampNow();

} // namespace mcdk
