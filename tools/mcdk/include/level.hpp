#pragma once

#include <cstdint>
#include <vector>

#include "settings.hpp"

namespace mcdk {

    [[nodiscard]] std::vector<uint8_t> createUserLevel(const WorldProjectConfig& config);

} // namespace mcdk
