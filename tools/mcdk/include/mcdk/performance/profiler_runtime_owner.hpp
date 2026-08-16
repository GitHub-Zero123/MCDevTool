#pragma once

#include <condition_variable>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include "profiler_service.hpp"

namespace mcdk::performance {

    class ProfilerServiceProvider {
    public:
        using ServicePtr = std::shared_ptr<ProfilerService>;
        using Factory    = std::function<std::expected<ServicePtr, ProfilerError>()>;

        explicit ProfilerServiceProvider(Factory factory);
        ~ProfilerServiceProvider();

        ProfilerServiceProvider(const ProfilerServiceProvider&)            = delete;
        ProfilerServiceProvider& operator=(const ProfilerServiceProvider&) = delete;
        ProfilerServiceProvider(ProfilerServiceProvider&&)                 = delete;
        ProfilerServiceProvider& operator=(ProfilerServiceProvider&&)      = delete;

        [[nodiscard]] std::expected<ServicePtr, ProfilerError> get();
        [[nodiscard]] bool                                     initialized() const;
        [[nodiscard]] bool                                     stopped() const;
        void                                                   shutdown() noexcept;

    private:
        mutable std::mutex           mutex_;
        std::condition_variable      condition_;
        Factory                      factory_;
        ServicePtr                   service_;
        std::optional<ProfilerError> lastError_;
        std::uint64_t                attemptGeneration_ = 0;
        bool                         initializing_      = false;
        bool                         stopped_           = false;
    };

    class ProfilerRuntimeOwner {
    public:
        explicit ProfilerRuntimeOwner(ProfilerServiceProvider::Factory factory);
        ~ProfilerRuntimeOwner() = default;

        ProfilerRuntimeOwner(const ProfilerRuntimeOwner&)            = delete;
        ProfilerRuntimeOwner& operator=(const ProfilerRuntimeOwner&) = delete;
        ProfilerRuntimeOwner(ProfilerRuntimeOwner&&)                 = delete;
        ProfilerRuntimeOwner& operator=(ProfilerRuntimeOwner&&)      = delete;

        [[nodiscard]] ProfilerServiceProvider&       provider() noexcept;
        [[nodiscard]] const ProfilerServiceProvider& provider() const noexcept;
        void                                         shutdown() noexcept;

    private:
        ProfilerServiceProvider provider_;
    };

} // namespace mcdk::performance
