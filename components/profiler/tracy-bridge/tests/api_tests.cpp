#include "mcdev_tracy_bridge.h"

#include <cassert>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

int main() {
    assert(mcdev_tracy_api_version() == 1);
    assert(std::strcmp(mcdev_tracy_protocol_version(), "0.11.1") == 0);
    assert(mcdev_tracy_get_status(nullptr) == -1);
    assert(mcdev_tracy_stop(nullptr) == -1);
    assert(mcdev_tracy_release(nullptr) == -1);

    const auto invalid = mcdev_tracy_start("127.0.0.1", 0, 10, 50, 160, "capture.tracy");
    assert(invalid == nullptr);
    const auto errorSize = mcdev_tracy_last_error_size();
    assert(errorSize > 1);
    std::vector<char> error(errorSize);
    assert(mcdev_tracy_copy_last_error(error.data(), error.size()) == 0);
    assert(std::string(error.data()).find("out of range") != std::string::npos);

    const auto first = mcdev_tracy_start("127.0.0.1", 1, 5, 50, 160, "cancelled.tracy");
    assert(first != nullptr);
    const auto concurrent = mcdev_tracy_start("127.0.0.1", 2, 5, 50, 160, "concurrent.tracy");
    assert(concurrent == nullptr);
    assert(mcdev_tracy_stop(first) == 0);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (mcdev_tracy_get_status(first) != MCDEV_TRACY_FAILED
        && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(mcdev_tracy_get_status(first) == MCDEV_TRACY_FAILED);
    assert(mcdev_tracy_error_size(first) > 1);
    assert(mcdev_tracy_release(first) == 0);

    mcdev_tracy_shutdown_all();
    return 0;
}
