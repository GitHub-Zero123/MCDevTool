#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace mcdev::tracy_bridge {

enum class Status : std::int32_t {
    Connecting = 1,
    Capturing = 2,
    Finalizing = 3,
    Completed = 4,
    Failed = 5
};

struct CaptureOptions {
    std::string address;
    std::uint16_t port;
    std::uint32_t maximumSeconds;
    std::uint32_t memoryLimitPercent;
    std::uint32_t maximumZones;
    std::string tracePath;
};

class CaptureSession final {
public:
    explicit CaptureSession(CaptureOptions options);
    ~CaptureSession();

    CaptureSession(const CaptureSession&) = delete;
    CaptureSession& operator=(const CaptureSession&) = delete;

    void start();
    void requestStop() noexcept;
    void join() noexcept;
    Status status() const noexcept;
    std::string result() const;
    std::string error() const;

private:
    void run() noexcept;
    void fail(std::string message) noexcept;

    CaptureOptions options_;
    std::atomic<Status> status_{Status::Connecting};
    std::atomic<bool> stopRequested_{false};
    mutable std::mutex valueMutex_;
    std::string result_;
    std::string error_;
    std::thread thread_;
};

} // namespace mcdev::tracy_bridge

