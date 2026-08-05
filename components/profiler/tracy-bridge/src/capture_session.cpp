#include "capture_session.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#endif

#include "TracyFileWrite.hpp"
#include "TracyMemory.hpp"
#include "TracyProtocol.hpp"
#include "TracySysUtil.hpp"
#include "TracyWorker.hpp"
#include "result_builder.hpp"

namespace mcdev::tracy_bridge {
namespace {

using Clock = std::chrono::steady_clock;

std::string handshakeError(std::uint8_t status) {
    switch (status) {
    case tracy::HandshakeProtocolMismatch:
        return "The game uses an incompatible Tracy protocol version (expected 0.11.1)";
    case tracy::HandshakeNotAvailable:
        return "The game Tracy endpoint is already connected to another profiler";
    case tracy::HandshakeDropped:
        return "The game disconnected during the Tracy handshake";
    default:
        return {};
    }
}

std::string tracePathForTracy(const std::string& utf8Path) {
#ifdef _WIN32
    const auto wideLength = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8Path.data(), static_cast<int>(utf8Path.size()), nullptr, 0
    );
    if (wideLength <= 0) throw std::runtime_error("The temporary Tracy path is not valid UTF-8");
    std::wstring widePath(static_cast<std::size_t>(wideLength), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8Path.data(), static_cast<int>(utf8Path.size()),
        widePath.data(), wideLength
    );

    const auto toSystemPath = [](const std::wstring& value) -> std::string {
        BOOL usedDefault = FALSE;
        const auto length = WideCharToMultiByte(
            CP_ACP, WC_NO_BEST_FIT_CHARS, value.data(), static_cast<int>(value.size()),
            nullptr, 0, nullptr, &usedDefault
        );
        if (length <= 0 || usedDefault) return {};
        std::string result(static_cast<std::size_t>(length), '\0');
        usedDefault = FALSE;
        WideCharToMultiByte(
            CP_ACP, WC_NO_BEST_FIT_CHARS, value.data(), static_cast<int>(value.size()),
            result.data(), length, nullptr, &usedDefault
        );
        return usedDefault ? std::string{} : result;
    };

    if (auto systemPath = toSystemPath(widePath); !systemPath.empty()) return systemPath;

    FILE* placeholder = nullptr;
    if (_wfopen_s(&placeholder, widePath.c_str(), L"wb") == 0 && placeholder) {
        fclose(placeholder);
        const auto shortLength = GetShortPathNameW(widePath.c_str(), nullptr, 0);
        if (shortLength > 1) {
            std::wstring shortPath(static_cast<std::size_t>(shortLength), L'\0');
            const auto written = GetShortPathNameW(widePath.c_str(), shortPath.data(), shortLength);
            DeleteFileW(widePath.c_str());
            if (written > 0 && written < shortLength) {
                shortPath.resize(written);
                if (auto systemPath = toSystemPath(shortPath); !systemPath.empty()) return systemPath;
            }
        } else {
            DeleteFileW(widePath.c_str());
        }
    }
    throw std::runtime_error("The temporary Tracy path cannot be represented by the Windows system code page");
#else
    return utf8Path;
#endif
}

} // namespace

CaptureSession::CaptureSession(CaptureOptions options) : options_(std::move(options)) {}

CaptureSession::~CaptureSession() {
    requestStop();
    join();
}

void CaptureSession::start() {
    thread_ = std::thread([this] { run(); });
}

void CaptureSession::requestStop() noexcept {
    stopRequested_.store(true, std::memory_order_relaxed);
}

void CaptureSession::join() noexcept {
    if (thread_.joinable()) thread_.join();
}

Status CaptureSession::status() const noexcept {
    return status_.load(std::memory_order_acquire);
}

std::string CaptureSession::result() const {
    std::lock_guard lock(valueMutex_);
    return result_;
}

std::string CaptureSession::error() const {
    std::lock_guard lock(valueMutex_);
    return error_;
}

void CaptureSession::run() noexcept {
    try {
        const auto memoryLimit = static_cast<std::int64_t>(options_.memoryLimitPercent)
            * tracy::GetPhysicalMemorySize() / 100;
        tracy::Worker worker(options_.address.c_str(), options_.port, memoryLimit);
        const auto connectStarted = Clock::now();
        while (!worker.HasData()) {
            if (stopRequested_.load(std::memory_order_relaxed)) {
                worker.Disconnect();
                throw std::runtime_error("Native profile capture was cancelled before connecting");
            }
            const auto error = handshakeError(worker.GetHandshakeStatus());
            if (!error.empty()) {
                worker.Disconnect();
                throw std::runtime_error(error);
            }
            if (Clock::now() - connectStarted >= std::chrono::seconds(15)) {
                worker.Disconnect();
                throw std::runtime_error("Timed out while connecting to the game Tracy endpoint");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }

        status_.store(Status::Capturing, std::memory_order_release);
        const auto capturedAt = Clock::now();
        while (worker.IsConnected()) {
            if (stopRequested_.load(std::memory_order_relaxed)
                || Clock::now() - capturedAt >= std::chrono::seconds(options_.maximumSeconds)) {
                worker.Disconnect();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        const auto completedAt = Clock::now();
        status_.store(Status::Finalizing, std::memory_order_release);

        const auto tracePath = tracePathForTracy(options_.tracePath);
        auto file = std::unique_ptr<tracy::FileWrite>(
            tracy::FileWrite::Open(tracePath.c_str(), tracy::FileCompression::Zstd, 3, 2)
        );
        if (!file) throw std::runtime_error("Unable to create the temporary Tracy capture file");
        worker.Write(*file, false);
        file->Finish();

        const double capturedSeconds = std::chrono::duration<double>(completedAt - capturedAt).count();
        std::string result = buildResultJson(worker, capturedSeconds, options_.maximumZones);
        {
            std::lock_guard lock(valueMutex_);
            result_ = std::move(result);
        }
        status_.store(Status::Completed, std::memory_order_release);
    } catch (const std::exception& error) {
        fail(error.what());
    } catch (...) {
        fail("Unknown native Tracy bridge failure");
    }
}

void CaptureSession::fail(std::string message) noexcept {
    try {
        std::lock_guard lock(valueMutex_);
        error_ = std::move(message);
    } catch (...) {
        // Keep the ABI failure-safe even under allocation failure.
    }
    status_.store(Status::Failed, std::memory_order_release);
}

} // namespace mcdev::tracy_bridge
