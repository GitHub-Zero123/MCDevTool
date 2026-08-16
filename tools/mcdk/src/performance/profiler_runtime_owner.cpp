#include <mcdk/performance/profiler_runtime_owner.hpp>

#include <exception>
#include <utility>

namespace mcdk::performance {

    ProfilerServiceProvider::ProfilerServiceProvider(Factory factory) : factory_(std::move(factory)) {}

    ProfilerServiceProvider::~ProfilerServiceProvider() { shutdown(); }

    std::expected<ProfilerServiceProvider::ServicePtr, ProfilerError> ProfilerServiceProvider::get() {
        std::unique_lock lock(mutex_);
        if (stopped_) {
            return std::unexpected(
                ProfilerError{
                    .code      = "PROFILER_SERVICE_STOPPED",
                    .message   = "The profiler runtime owner has already been shut down.",
                    .retryable = false,
                }
            );
        }
        if (service_) {
            return service_;
        }
        if (initializing_) {
            const auto observedGeneration = attemptGeneration_;
            condition_.wait(lock, [this] { return stopped_ || !initializing_; });
            if (stopped_) {
                return std::unexpected(
                    ProfilerError{
                        .code      = "PROFILER_SERVICE_STOPPED",
                        .message   = "The profiler runtime owner has already been shut down.",
                        .retryable = false,
                    }
                );
            }
            if (service_) {
                return service_;
            }
            if (lastError_ && observedGeneration == attemptGeneration_) {
                return std::unexpected(*lastError_);
            }
        }
        if (!factory_) {
            return std::unexpected(
                ProfilerError{
                    .code      = "PROFILER_FACTORY_UNAVAILABLE",
                    .message   = "No profiler service factory is configured.",
                    .retryable = false,
                }
            );
        }

        Factory factory;
        try {
            factory = factory_;
        } catch (const std::exception& error) {
            return std::unexpected(
                ProfilerError{
                    .code      = "PROFILER_FACTORY_EXCEPTION",
                    .message   = error.what(),
                    .retryable = true,
                }
            );
        } catch (...) {
            return std::unexpected(
                ProfilerError{
                    .code      = "PROFILER_FACTORY_EXCEPTION",
                    .message   = "The profiler service factory raised an unknown exception.",
                    .retryable = true,
                }
            );
        }

        initializing_ = true;
        ++attemptGeneration_;
        lock.unlock();

        std::expected<ServicePtr, ProfilerError> created = std::unexpected(
            ProfilerError{
                .code      = "PROFILER_FACTORY_EXCEPTION",
                .message   = "The profiler service factory raised an unknown exception.",
                .retryable = true,
            }
        );
        try {
            created = factory();
            if (created && !*created) {
                created = std::unexpected(
                    ProfilerError{
                        .code      = "PROFILER_FACTORY_FAILED",
                        .message   = "The profiler service factory returned an empty service.",
                        .retryable = true,
                    }
                );
            }
        } catch (const std::exception& error) {
            created = std::unexpected(
                ProfilerError{
                    .code      = "PROFILER_FACTORY_EXCEPTION",
                    .message   = error.what(),
                    .retryable = true,
                }
            );
        } catch (...) {}

        lock.lock();
        if (stopped_) {
            auto serviceToStop = created ? std::move(*created) : ServicePtr{};
            initializing_      = false;
            condition_.notify_all();
            lock.unlock();
            if (serviceToStop) {
                serviceToStop->shutdown();
            }
            return std::unexpected(
                ProfilerError{
                    .code      = "PROFILER_SERVICE_STOPPED",
                    .message   = "The profiler runtime owner was shut down during initialization.",
                    .retryable = false,
                }
            );
        }

        if (created) {
            service_ = std::move(*created);
            lastError_.reset();
        } else {
            lastError_ = created.error();
        }
        initializing_ = false;
        condition_.notify_all();
        if (service_) {
            return service_;
        }
        return std::unexpected(*lastError_);
    }

    bool ProfilerServiceProvider::initialized() const {
        std::lock_guard lock(mutex_);
        return static_cast<bool>(service_);
    }

    bool ProfilerServiceProvider::stopped() const {
        std::lock_guard lock(mutex_);
        return stopped_;
    }

    void ProfilerServiceProvider::shutdown() noexcept {
        ServicePtr service;
        {
            std::unique_lock lock(mutex_);
            if (stopped_) {
                condition_.wait(lock, [this] { return !initializing_; });
                return;
            }
            stopped_ = true;
            factory_ = {};
            condition_.notify_all();
            condition_.wait(lock, [this] { return !initializing_; });
            service = std::move(service_);
        }
        if (service) {
            service->shutdown();
        }
    }

    ProfilerRuntimeOwner::ProfilerRuntimeOwner(ProfilerServiceProvider::Factory factory)
    : provider_(std::move(factory)) {}

    ProfilerServiceProvider& ProfilerRuntimeOwner::provider() noexcept { return provider_; }

    const ProfilerServiceProvider& ProfilerRuntimeOwner::provider() const noexcept { return provider_; }

    void ProfilerRuntimeOwner::shutdown() noexcept { provider_.shutdown(); }

} // namespace mcdk::performance
