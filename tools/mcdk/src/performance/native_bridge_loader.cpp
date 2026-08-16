#include <mcdk/performance/native_bridge_loader.hpp>

#include <array>
#include <fstream>
#include <mutex>
#include <string_view>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <bcrypt.h>
#include <iphlpapi.h>
#endif

namespace mcdk::performance {
namespace {

    constexpr std::uint16_t TracyPortStart = 8086;
    constexpr std::uint16_t TracyPortEnd   = 8105;
    constexpr std::size_t   MaximumResultBytes = 16 * 1024 * 1024;

    ProfilerError nativeError(std::string code, std::string message, bool retryable = false) {
        if (code.size() > 128) code.resize(128);
        if (message.size() > 4096) message.resize(4096);
        return {.code = std::move(code), .message = std::move(message), .retryable = retryable};
    }

#ifdef _WIN32
    std::uint64_t fileTimeValue(const FILETIME& value) {
        return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
    }

    std::expected<std::uint64_t, ProfilerError> processIdentity(std::uint32_t pid) {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
        if (!process) {
            return std::unexpected(nativeError("GAME_PROCESS_UNAVAILABLE", "The Minecraft process is not available.", true));
        }
        FILETIME created{}, exited{}, kernel{}, user{};
        const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
        const bool times = GetProcessTimes(process, &created, &exited, &kernel, &user) != FALSE;
        CloseHandle(process);
        if (!alive || !times) {
            return std::unexpected(nativeError("GAME_PROCESS_UNAVAILABLE", "The Minecraft process has exited.", true));
        }
        return fileTimeValue(created);
    }

    std::expected<std::vector<std::uint16_t>, ProfilerError> listeningPorts(std::uint32_t pid) {
        ULONG size = 0;
        const auto first = GetExtendedTcpTable(
            nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0
        );
        if (first != ERROR_INSUFFICIENT_BUFFER) {
            return std::unexpected(nativeError("TRACY_DISCOVERY_FAILED", "Unable to size the Windows TCP listener table.", true));
        }
        std::vector<std::byte> buffer(size);
        auto* table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buffer.data());
        const auto loaded = GetExtendedTcpTable(
            table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_LISTENER, 0
        );
        if (loaded != NO_ERROR) {
            return std::unexpected(nativeError("TRACY_DISCOVERY_FAILED", "Unable to read the Windows TCP listener table.", true));
        }
        std::vector<std::uint16_t> ports;
        for (DWORD index = 0; index < table->dwNumEntries; ++index) {
            const auto& row  = table->table[index];
            const auto  port = ntohs(static_cast<u_short>(row.dwLocalPort));
            if (row.dwOwningPid == pid && port >= TracyPortStart && port <= TracyPortEnd) {
                ports.push_back(port);
            }
        }
        std::sort(ports.begin(), ports.end());
        ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
        return ports;
    }

    std::expected<bool, ProfilerError> endpointConnected(std::uint32_t pid, std::uint16_t port) {
        ULONG size = 0;
        const auto first = GetExtendedTcpTable(
            nullptr, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0
        );
        if (first != ERROR_INSUFFICIENT_BUFFER) {
            return std::unexpected(nativeError("TRACY_DISCOVERY_FAILED", "Unable to size the Windows TCP connection table.", true));
        }
        std::vector<std::byte> buffer(size);
        auto* table = reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(buffer.data());
        const auto loaded = GetExtendedTcpTable(
            table, &size, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0
        );
        if (loaded != NO_ERROR) {
            return std::unexpected(nativeError("TRACY_DISCOVERY_FAILED", "Unable to read the Windows TCP connection table.", true));
        }
        for (DWORD index = 0; index < table->dwNumEntries; ++index) {
            const auto& row = table->table[index];
            if (row.dwOwningPid == pid && row.dwState == MIB_TCP_STATE_ESTAB
                && ntohs(static_cast<u_short>(row.dwLocalPort)) == port) {
                return true;
            }
        }
        return false;
    }

    std::expected<std::string, ProfilerError> sha256File(const std::filesystem::path& path) {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        std::vector<UCHAR> hashObject;
        std::array<UCHAR, 32> digest{};
        auto cleanup = [&] {
            if (hash) BCryptDestroyHash(hash);
            if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        };
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
            cleanup();
            return std::unexpected(nativeError("NATIVE_COMPONENT_HASH_FAILED", "Unable to initialize SHA-256 verification."));
        }
        DWORD objectBytes = 0;
        DWORD copied = 0;
        if (BCryptGetProperty(
                algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &copied, 0
            ) < 0) {
            cleanup();
            return std::unexpected(nativeError("NATIVE_COMPONENT_HASH_FAILED", "Unable to inspect SHA-256 provider."));
        }
        hashObject.resize(objectBytes);
        if (BCryptCreateHash(algorithm, &hash, hashObject.data(), objectBytes, nullptr, 0, 0) < 0) {
            cleanup();
            return std::unexpected(nativeError("NATIVE_COMPONENT_HASH_FAILED", "Unable to create SHA-256 state."));
        }
        std::ifstream input(path, std::ios::binary);
        std::array<char, 64 * 1024> chunk{};
        while (input) {
            input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
            const auto count = input.gcount();
            if (count > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(chunk.data()), static_cast<ULONG>(count), 0) < 0) {
                cleanup();
                return std::unexpected(nativeError("NATIVE_COMPONENT_HASH_FAILED", "Unable to hash the Native component."));
            }
        }
        if (!input.eof() || BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
            cleanup();
            return std::unexpected(nativeError("NATIVE_COMPONENT_HASH_FAILED", "Unable to finish Native component verification."));
        }
        cleanup();
        static constexpr char Hex[] = "0123456789abcdef";
        std::string result;
        result.reserve(digest.size() * 2);
        for (const auto byte : digest) {
            result.push_back(Hex[byte >> 4]);
            result.push_back(Hex[byte & 0x0f]);
        }
        return result;
    }
#endif

} // namespace

class NativeBridgeLoader::Impl {
public:
#ifdef _WIN32
    using Handle = void*;
    using ApiVersion = std::uint32_t (*)();
    using ProtocolVersion = const char* (*)();
    using Start = Handle (*)(const char*, std::uint16_t, std::uint32_t, std::uint32_t, std::uint32_t, const char*);
    using Status = std::int32_t (*)(Handle);
    using Stop = std::int32_t (*)(Handle);
    using Size = std::size_t (*)(Handle);
    using Copy = std::int32_t (*)(Handle, char*, std::size_t);
    using LastSize = std::size_t (*)();
    using LastCopy = std::int32_t (*)(char*, std::size_t);
    using Release = std::int32_t (*)(Handle);
    using Shutdown = void (*)();

    HMODULE module = nullptr;
    ApiVersion apiVersion = nullptr;
    ProtocolVersion protocolVersion = nullptr;
    Start start = nullptr;
    Status status = nullptr;
    Stop stop = nullptr;
    Size resultSize = nullptr;
    Copy copyResult = nullptr;
    Size errorSize = nullptr;
    Copy copyError = nullptr;
    LastSize lastErrorSize = nullptr;
    LastCopy copyLastError = nullptr;
    Release release = nullptr;
    Shutdown shutdownAll = nullptr;
#endif
    explicit Impl(std::filesystem::path executable)
    : executableDirectory(std::move(executable)) {}

    std::filesystem::path executableDirectory;
    mutable std::mutex mutex;
    bool attempted = false;
    std::string reason = "Native component has not been probed.";
};

NativeBridgeLoader::NativeBridgeLoader(std::filesystem::path executableDirectory)
: impl_(std::make_unique<Impl>(std::move(executableDirectory))) {}

NativeBridgeLoader::~NativeBridgeLoader() { shutdown(); }

std::expected<void, ProfilerError> NativeBridgeLoader::initialize() {
    std::lock_guard lock(impl_->mutex);
#ifdef _WIN32
    if (impl_->module) return {};
#endif
    impl_->attempted = true;
    try {
#ifndef _WIN32
        impl_->reason = "Native profiling requires Windows x64.";
        return std::unexpected(nativeError("NATIVE_PLATFORM_UNSUPPORTED", impl_->reason));
#else
        if (sizeof(void*) != 8) {
            impl_->reason = "Native profiling requires a 64-bit MCDK process.";
            return std::unexpected(nativeError("NATIVE_PLATFORM_UNSUPPORTED", impl_->reason));
        }
        const auto libraryPath = impl_->executableDirectory / "mcdev-tracy-bridge.dll";
        if (!std::filesystem::is_regular_file(libraryPath)) {
            impl_->reason = "Native profiler DLL is missing beside mcdk.exe.";
            return std::unexpected(nativeError("NATIVE_COMPONENT_MISSING", impl_->reason, true));
        }
        impl_->module = LoadLibraryExW(
            libraryPath.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32
        );
        if (!impl_->module) {
            impl_->reason = "Windows could not load the Native profiler DLL.";
            return std::unexpected(nativeError("NATIVE_COMPONENT_LOAD_FAILED", impl_->reason, true));
        }
        const auto resolve = [this](auto& target, const char* name) {
            target = reinterpret_cast<std::remove_reference_t<decltype(target)>>(GetProcAddress(impl_->module, name));
            return target != nullptr;
        };
        const bool complete = resolve(impl_->apiVersion, "mcdev_tracy_api_version")
            && resolve(impl_->protocolVersion, "mcdev_tracy_protocol_version")
            && resolve(impl_->start, "mcdev_tracy_start") && resolve(impl_->status, "mcdev_tracy_get_status")
            && resolve(impl_->stop, "mcdev_tracy_stop") && resolve(impl_->resultSize, "mcdev_tracy_result_size")
            && resolve(impl_->copyResult, "mcdev_tracy_copy_result") && resolve(impl_->errorSize, "mcdev_tracy_error_size")
            && resolve(impl_->copyError, "mcdev_tracy_copy_error")
            && resolve(impl_->lastErrorSize, "mcdev_tracy_last_error_size")
            && resolve(impl_->copyLastError, "mcdev_tracy_copy_last_error")
            && resolve(impl_->release, "mcdev_tracy_release")
            && resolve(impl_->shutdownAll, "mcdev_tracy_shutdown_all");
        const char* protocol = complete ? impl_->protocolVersion() : nullptr;
        if (!complete || impl_->apiVersion() != 1 || !protocol || std::string_view(protocol) != "0.11.1") {
            FreeLibrary(impl_->module);
            impl_->module = nullptr;
            impl_->reason = "Native profiler DLL exports or protocol version are incompatible.";
            return std::unexpected(nativeError("NATIVE_COMPONENT_INCOMPATIBLE", impl_->reason));
        }
        impl_->reason = "Native profiler DLL is loaded and API/protocol compatible; endpoint discovery remains deferred.";
        return {};
#endif
    } catch (const std::exception& error) {
#ifdef _WIN32
        if (impl_->module) {
            FreeLibrary(impl_->module);
            impl_->module = nullptr;
        }
#endif
        impl_->reason = std::string("Native component probe failed: ") + error.what();
        return std::unexpected(nativeError("NATIVE_COMPONENT_INVALID", impl_->reason, true));
    } catch (...) {
#ifdef _WIN32
        if (impl_->module) {
            FreeLibrary(impl_->module);
            impl_->module = nullptr;
        }
#endif
        impl_->reason = "Native component probe failed with an unknown error.";
        return std::unexpected(nativeError("NATIVE_COMPONENT_INVALID", impl_->reason, true));
    }
}

bool NativeBridgeLoader::available() const noexcept {
    std::lock_guard lock(impl_->mutex);
#ifdef _WIN32
    return impl_->module != nullptr;
#else
    return false;
#endif
}

std::string NativeBridgeLoader::availabilityReason() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->reason;
}

std::expected<NativeEndpoint, ProfilerError> NativeBridgeLoader::discover(std::uint32_t expectedPid) const {
#ifndef _WIN32
    (void)expectedPid;
    return std::unexpected(nativeError("NATIVE_PLATFORM_UNSUPPORTED", "Native profiling requires Windows x64."));
#else
    if (!available()) return std::unexpected(nativeError("NATIVE_COMPONENT_UNAVAILABLE", availabilityReason(), true));
    if (expectedPid == 0) {
        return std::unexpected(nativeError("GAME_PROCESS_UNAVAILABLE", "Minecraft has not been started.", true));
    }
    const auto identity = processIdentity(expectedPid);
    if (!identity) return std::unexpected(identity.error());
    const auto ports = listeningPorts(expectedPid);
    if (!ports) return std::unexpected(ports.error());
    if (ports->empty()) {
        return std::unexpected(nativeError("TRACY_ENDPOINT_NOT_FOUND", "No Tracy listener owned by the current game PID was found in ports 8086..8105.", true));
    }
    if (ports->size() != 1) {
        return std::unexpected(nativeError("TRACY_ENDPOINT_AMBIGUOUS", "Multiple Tracy listeners are owned by the current game PID; refusing to guess a port."));
    }
    const auto connected = endpointConnected(expectedPid, ports->front());
    if (!connected) return std::unexpected(connected.error());
    if (*connected) {
        return std::unexpected(nativeError(
            "TRACY_ENDPOINT_BUSY",
            "The current game Tracy endpoint is already connected to another profiler.",
            true
        ));
    }
    return NativeEndpoint{expectedPid, ports->front(), *identity};
#endif
}

bool NativeBridgeLoader::validateProcess(const NativeEndpoint& endpoint) const noexcept {
#ifdef _WIN32
    try {
        const auto identity = processIdentity(endpoint.pid);
        return identity && *identity == endpoint.identity;
    } catch (...) { return false; }
#else
    (void)endpoint;
    return false;
#endif
}

std::expected<NativeCaptureHandle, ProfilerError> NativeBridgeLoader::start(
    std::uint32_t expectedPid,
    std::chrono::seconds duration,
    const std::filesystem::path& tracePath
) {
#ifndef _WIN32
    (void)expectedPid; (void)duration; (void)tracePath;
    return std::unexpected(nativeError("NATIVE_PLATFORM_UNSUPPORTED", "Native profiling requires Windows x64."));
#else
    auto endpoint = discover(expectedPid);
    if (!endpoint) return std::unexpected(endpoint.error());
    const auto secondIdentity = processIdentity(expectedPid);
    const auto secondPorts = listeningPorts(expectedPid);
    const auto connected = endpointConnected(expectedPid, endpoint->port);
    if (!secondIdentity || !secondPorts || *secondIdentity != endpoint->identity
        || std::find(secondPorts->begin(), secondPorts->end(), endpoint->port) == secondPorts->end()) {
        return std::unexpected(nativeError("TRACY_ENDPOINT_CHANGED", "The game process or Tracy listener changed during discovery.", true));
    }
    if (!connected) return std::unexpected(connected.error());
    if (*connected) {
        return std::unexpected(nativeError(
            "TRACY_ENDPOINT_BUSY",
            "The current game Tracy endpoint became busy before capture could start.",
            true
        ));
    }
    std::filesystem::create_directories(tracePath.parent_path());
    const auto traceUtf8 = tracePath.generic_u8string();
    auto handle = impl_->start(
        "127.0.0.1", endpoint->port, static_cast<std::uint32_t>(duration.count()), 50, 512,
        reinterpret_cast<const char*>(traceUtf8.c_str())
    );
    if (!handle) {
        std::string message = "Unable to start the Native Tracy capture.";
        const auto size = impl_->lastErrorSize();
        if (size > 1 && size <= 64 * 1024) {
            std::vector<char> value(size);
            if (impl_->copyLastError(value.data(), value.size()) == 0) message.assign(value.data());
        }
        return std::unexpected(nativeError("NATIVE_CAPTURE_START_FAILED", std::move(message), true));
    }
    return NativeCaptureHandle{handle, *endpoint};
#endif
}

int NativeBridgeLoader::status(const NativeCaptureHandle& capture) const noexcept {
#ifdef _WIN32
    return capture.value && impl_->module ? impl_->status(capture.value) : -1;
#else
    (void)capture; return -1;
#endif
}

void NativeBridgeLoader::stop(const NativeCaptureHandle& capture) const noexcept {
#ifdef _WIN32
    if (capture.value && impl_->module) (void)impl_->stop(capture.value);
#else
    (void)capture;
#endif
}

std::expected<std::string, ProfilerError> NativeBridgeLoader::result(const NativeCaptureHandle& capture) const {
#ifndef _WIN32
    (void)capture;
    return std::unexpected(nativeError("NATIVE_PLATFORM_UNSUPPORTED", "Native profiling requires Windows x64."));
#else
    const auto size = capture.value && impl_->module ? impl_->resultSize(capture.value) : 0;
    if (size <= 1 || size > MaximumResultBytes) {
        return std::unexpected(nativeError("NATIVE_RESULT_INVALID", "Native profiler result is empty or exceeds the 16 MiB safety limit."));
    }
    std::vector<char> value(size);
    if (impl_->copyResult(capture.value, value.data(), value.size()) != 0) {
        return std::unexpected(nativeError("NATIVE_RESULT_INVALID", "Native profiler result could not be copied."));
    }
    return std::string(value.data());
#endif
}

std::string NativeBridgeLoader::error(const NativeCaptureHandle& capture) const {
#ifdef _WIN32
    const auto size = capture.value && impl_->module ? impl_->errorSize(capture.value) : 0;
    if (size <= 1 || size > 64 * 1024) return "Native Tracy capture failed.";
    std::vector<char> value(size);
    return impl_->copyError(capture.value, value.data(), value.size()) == 0
         ? std::string(value.data()) : "Native Tracy capture failed.";
#else
    (void)capture; return "Native profiling requires Windows x64.";
#endif
}

void NativeBridgeLoader::release(NativeCaptureHandle& capture) noexcept {
#ifdef _WIN32
    if (capture.value && impl_->module) (void)impl_->release(capture.value);
#endif
    capture.value = nullptr;
}

void NativeBridgeLoader::shutdown() noexcept {
    std::lock_guard lock(impl_->mutex);
#ifdef _WIN32
    if (impl_->module) {
        impl_->shutdownAll();
        FreeLibrary(impl_->module);
        impl_->module = nullptr;
    }
#endif
}

std::expected<std::string, ProfilerError> calculateFileSha256(const std::filesystem::path& path) {
#ifdef _WIN32
    return sha256File(path);
#else
    (void)path;
    return std::unexpected(nativeError("SHA256_UNAVAILABLE", "SHA-256 artifact hashing is unavailable on this platform."));
#endif
}

} // namespace mcdk::performance
