#include "mcdev_tracy_bridge.h"

#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "capture_session.hpp"

namespace {

using mcdev::tracy_bridge::CaptureOptions;
using mcdev::tracy_bridge::CaptureSession;

std::mutex sessionsMutex;
std::unordered_map<mcdev_tracy_handle, std::shared_ptr<CaptureSession>> sessions;
std::weak_ptr<CaptureSession> activeSession;
thread_local std::string lastError;

void setLastError(std::string value) noexcept {
    try { lastError = std::move(value); } catch (...) {}
}

std::shared_ptr<CaptureSession> findSession(mcdev_tracy_handle handle) {
    std::lock_guard lock(sessionsMutex);
    const auto found = sessions.find(handle);
    return found == sessions.end() ? nullptr : found->second;
}

bool isTerminal(const std::shared_ptr<CaptureSession>& session) {
    if (!session) return true;
    const auto status = session->status();
    return status == mcdev::tracy_bridge::Status::Completed
        || status == mcdev::tracy_bridge::Status::Failed;
}

int copyString(const std::string& value, char* destination, std::size_t capacity) {
    if (!destination || capacity < value.size() + 1) return -1;
    std::memcpy(destination, value.data(), value.size());
    destination[value.size()] = '\0';
    return 0;
}

} // namespace

extern "C" {

uint32_t mcdev_tracy_api_version(void) { return 1; }
const char* mcdev_tracy_protocol_version(void) { return "0.11.1"; }

mcdev_tracy_handle mcdev_tracy_start(
    const char* addressUtf8,
    uint16_t port,
    uint32_t maximumSeconds,
    uint32_t memoryLimitPercent,
    uint32_t maximumZones,
    const char* tracePathUtf8
) {
    try {
        if (!addressUtf8 || !*addressUtf8 || !tracePathUtf8 || !*tracePathUtf8) {
            setLastError("Address and trace path are required");
            return nullptr;
        }
        if (port == 0 || maximumSeconds == 0 || maximumSeconds > 3600
            || memoryLimitPercent == 0 || memoryLimitPercent > 95
            || maximumZones == 0 || maximumZones > 10000) {
            setLastError("Native Tracy capture options are out of range");
            return nullptr;
        }

        std::lock_guard lock(sessionsMutex);
        if (const auto active = activeSession.lock(); !isTerminal(active)) {
            setLastError("Another native Tracy capture is already active in this extension host");
            return nullptr;
        }
        auto session = std::make_shared<CaptureSession>(CaptureOptions{
            addressUtf8, port, maximumSeconds, memoryLimitPercent, maximumZones, tracePathUtf8
        });
        const auto handle = static_cast<mcdev_tracy_handle>(session.get());
        sessions.emplace(handle, session);
        activeSession = session;
        try {
            session->start();
        } catch (...) {
            sessions.erase(handle);
            activeSession.reset();
            throw;
        }
        lastError.clear();
        return handle;
    } catch (const std::exception& error) {
        setLastError(error.what());
        return nullptr;
    } catch (...) {
        setLastError("Unknown error while starting native Tracy capture");
        return nullptr;
    }
}

int32_t mcdev_tracy_get_status(mcdev_tracy_handle handle) {
    try {
        const auto session = findSession(handle);
        return session ? static_cast<int32_t>(session->status()) : -1;
    } catch (...) { return -1; }
}

int32_t mcdev_tracy_stop(mcdev_tracy_handle handle) {
    try {
        const auto session = findSession(handle);
        if (!session) return -1;
        session->requestStop();
        return 0;
    } catch (...) { return -1; }
}

size_t mcdev_tracy_result_size(mcdev_tracy_handle handle) {
    try {
        const auto session = findSession(handle);
        if (!session) return 0;
        const auto value = session->result();
        return value.empty() ? 0 : value.size() + 1;
    } catch (...) { return 0; }
}

int32_t mcdev_tracy_copy_result(mcdev_tracy_handle handle, char* destination, size_t capacity) {
    try {
        const auto session = findSession(handle);
        return session ? copyString(session->result(), destination, capacity) : -1;
    } catch (...) { return -1; }
}

size_t mcdev_tracy_error_size(mcdev_tracy_handle handle) {
    try {
        const auto session = findSession(handle);
        if (!session) return 0;
        const auto value = session->error();
        return value.empty() ? 0 : value.size() + 1;
    } catch (...) { return 0; }
}

int32_t mcdev_tracy_copy_error(mcdev_tracy_handle handle, char* destination, size_t capacity) {
    try {
        const auto session = findSession(handle);
        return session ? copyString(session->error(), destination, capacity) : -1;
    } catch (...) { return -1; }
}

size_t mcdev_tracy_last_error_size(void) {
    try { return lastError.empty() ? 0 : lastError.size() + 1; } catch (...) { return 0; }
}

int32_t mcdev_tracy_copy_last_error(char* destination, size_t capacity) {
    try { return copyString(lastError, destination, capacity); } catch (...) { return -1; }
}

int32_t mcdev_tracy_release(mcdev_tracy_handle handle) {
    try {
        std::shared_ptr<CaptureSession> session;
        {
            std::lock_guard lock(sessionsMutex);
            const auto found = sessions.find(handle);
            if (found == sessions.end()) return -1;
            session = std::move(found->second);
            sessions.erase(found);
        }
        session->requestStop();
        session->join();
        return 0;
    } catch (...) {
        return -1;
    }
}

void mcdev_tracy_shutdown_all(void) {
    try {
        std::vector<std::shared_ptr<CaptureSession>> values;
        {
            std::lock_guard lock(sessionsMutex);
            values.reserve(sessions.size());
            for (auto& entry : sessions) values.push_back(std::move(entry.second));
            sessions.clear();
            activeSession.reset();
        }
        for (const auto& session : values) session->requestStop();
        for (const auto& session : values) session->join();
    } catch (...) {
        // No exception may cross the C boundary. Existing CaptureSession destructors still request stop and join.
    }
}

} // extern "C"
