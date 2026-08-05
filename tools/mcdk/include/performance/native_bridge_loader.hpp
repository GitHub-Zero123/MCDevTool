#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>

#include "profiler_types.hpp"

namespace mcdk::performance {

    struct NativeEndpoint {
        std::uint32_t pid      = 0;
        std::uint16_t port     = 0;
        std::uint64_t identity = 0;
    };

    struct NativeCaptureHandle {
        void*          value = nullptr;
        NativeEndpoint endpoint;
    };

    class NativeBridgeLoader final {
    public:
        explicit NativeBridgeLoader(std::filesystem::path executableDirectory);
        ~NativeBridgeLoader();

        NativeBridgeLoader(const NativeBridgeLoader&)            = delete;
        NativeBridgeLoader& operator=(const NativeBridgeLoader&) = delete;

        [[nodiscard]] std::expected<void, ProfilerError> initialize();
        [[nodiscard]] bool                                available() const noexcept;
        [[nodiscard]] std::string                         availabilityReason() const;
        [[nodiscard]] std::expected<NativeEndpoint, ProfilerError> discover(std::uint32_t expectedPid) const;
        [[nodiscard]] bool validateProcess(const NativeEndpoint& endpoint) const noexcept;
        [[nodiscard]] std::expected<NativeCaptureHandle, ProfilerError> start(
            std::uint32_t                expectedPid,
            std::chrono::seconds         duration,
            const std::filesystem::path& tracePath
        );
        [[nodiscard]] int                                status(const NativeCaptureHandle& capture) const noexcept;
        void                                             stop(const NativeCaptureHandle& capture) const noexcept;
        [[nodiscard]] std::expected<std::string, ProfilerError>
        result(const NativeCaptureHandle& capture) const;
        [[nodiscard]] std::string error(const NativeCaptureHandle& capture) const;
        void                      release(NativeCaptureHandle& capture) noexcept;
        void                      shutdown() noexcept;

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
    };

    [[nodiscard]] std::expected<std::string, ProfilerError>
    calculateFileSha256(const std::filesystem::path& path);

} // namespace mcdk::performance
