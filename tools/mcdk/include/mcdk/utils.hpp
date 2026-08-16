#pragma once

#include <string>
#include <string_view>

namespace mcdk {

    void               stringReplace(std::string& value, const std::string& from, const std::string& to);
    [[nodiscard]] bool containsIgnoreCase(std::string_view text, std::string_view pattern);

} // namespace mcdk
