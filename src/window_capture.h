#pragma once

#ifdef _WIN32
#include <windows.h>
#include <cstdint>
#include <optional>
#include <vector>

namespace MCDevTool::Style::Detail {
    std::optional<std::vector<uint8_t>> captureWindow480p(HWND hwnd);
}
#endif
