#include <performance/profiler_types.hpp>

namespace mcdk::performance {

    const char* toString(ProfilerKind value) noexcept {
        switch (value) {
        case ProfilerKind::PythonCpu:
            return "python.cpu";
        case ProfilerKind::PythonMemory:
            return "python.memory";
        case ProfilerKind::NativeCpu:
            return "native.cpu";
        }
        return "unknown";
    }

    const char* toString(JobState value) noexcept {
        switch (value) {
        case JobState::Created:
            return "created";
        case JobState::Starting:
            return "starting";
        case JobState::Running:
            return "running";
        case JobState::Finalizing:
            return "finalizing";
        case JobState::Persisting:
            return "persisting";
        case JobState::CleanupPending:
            return "cleanup_pending";
        case JobState::Completed:
            return "completed";
        case JobState::Failed:
            return "failed";
        case JobState::Discarded:
            return "discarded";
        case JobState::Aborted:
            return "aborted";
        }
        return "unknown";
    }

} // namespace mcdk::performance
