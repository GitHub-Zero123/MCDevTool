#pragma once

#include <cstdint>
#include <string>

namespace tracy { class Worker; }

namespace mcdev::tracy_bridge {

std::string buildResultJson(
    tracy::Worker& worker,
    double capturedSeconds,
    std::uint32_t maximumZones,
    bool captureTruncated
);

} // namespace mcdev::tracy_bridge
