#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace mcdk::performance {

    using JobId = std::string;

    enum class ProfilerKind {
        PythonCpu,
        PythonMemory,
        NativeCpu,
    };

    enum class ProfileTarget {
        Client,
        Server,
        All,
    };

    enum class ProfileClock {
        Cpu,
        Wall,
    };

    enum class JobState {
        Created,
        Starting,
        Running,
        Finalizing,
        Persisting,
        CleanupPending,
        Completed,
        Failed,
        Discarded,
        Aborted,
    };

    struct ProfilerError {
        std::string code;
        std::string message;
        bool        retryable = false;
    };

    struct StartRequest {
        ProfilerKind         kind   = ProfilerKind::PythonCpu;
        ProfileTarget        target = ProfileTarget::Client;
        ProfileClock         clock  = ProfileClock::Wall;
        std::chrono::seconds duration{15};
    };

    struct JobSnapshot {
        JobId        id;
        ProfilerKind kind    = ProfilerKind::PythonCpu;
        JobState     state   = JobState::Created;
        bool         partial = false;
        std::string  statusMessage;
    };

    using ProfilerFieldValue = std::variant<std::int64_t, double, bool, std::string>;

    struct ProfilerField {
        ProfilerFieldValue value;
        std::string        unit;
    };

    struct QueryRecord {
        std::string                          id;
        std::map<std::string, ProfilerField> fields;
    };

    struct QueryRequest {
        JobId                      jobId;
        std::string                view;
        std::optional<std::string> filter;
        std::string                sort;
        bool                       descending = true;
        std::size_t                limit      = 20;
        std::optional<std::string> cursor;
    };

    struct QueryPage {
        std::vector<QueryRecord>   records;
        std::size_t                totalAvailable = 0;
        bool                       truncated      = false;
        std::optional<std::string> nextCursor;
    };

    struct DetailRequest {
        JobId       jobId;
        std::string view;
        std::string recordId;
    };

    struct DetailResult {
        QueryRecord              record;
        std::vector<QueryRecord> related;
    };

    struct HistoryRequest {
        std::size_t                limit = 20;
        std::optional<std::string> cursor;
    };

    struct HistoryPage {
        std::vector<JobSnapshot>   jobs;
        std::optional<std::string> nextCursor;
    };

    enum class ExportFormat {
        Markdown,
        Svg,
    };

    struct ExportRequest {
        JobId        jobId;
        ExportFormat format = ExportFormat::Markdown;
    };

    struct ExportResult {
        std::filesystem::path path;
        std::uintmax_t        size = 0;
        std::string           sha256;
    };

    struct CleanupRequest {
        bool dryRun = false;
    };

    struct CleanupResult {
        std::size_t    removedJobs  = 0;
        std::uintmax_t removedBytes = 0;
    };

    struct DoctorRequest {
        std::optional<ProfilerKind> kind;
        bool                        deep = false;
    };

    struct Capability {
        ProfilerKind kind      = ProfilerKind::PythonCpu;
        bool         available = false;
        std::string  reason;
    };

    struct Capabilities {
        std::vector<Capability>  entries;
        std::vector<std::string> warnings;
    };

    [[nodiscard]] const char* toString(ProfilerKind value) noexcept;
    [[nodiscard]] const char* toString(JobState value) noexcept;

} // namespace mcdk::performance
