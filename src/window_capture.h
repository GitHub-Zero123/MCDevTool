#pragma once

#ifdef _WIN32
#include <windows.h>
#include <cstdint>
#include <expected>
#include <vector>

#include <mcdevtool/style.h>

namespace MCDevTool::Style::Detail {
    std::expected<std::vector<uint8_t>, CaptureError> captureWindowJpeg(HWND hwnd, CaptureOptions options = {});
}
#endif
