#pragma once

#include <expected>

#include "profiler_types.hpp"

namespace mcdk::performance {

    class ProfilerService {
    public:
        virtual ~ProfilerService() = default;

        [[nodiscard]] virtual std::expected<JobSnapshot, ProfilerError>  start(const StartRequest& request)         = 0;
        [[nodiscard]] virtual std::expected<JobSnapshot, ProfilerError>  status(const JobId& id) const              = 0;
        [[nodiscard]] virtual std::expected<JobSnapshot, ProfilerError>  stop(const JobId& id)                      = 0;
        [[nodiscard]] virtual std::expected<void, ProfilerError>         discard(const JobId& id)                   = 0;
        [[nodiscard]] virtual std::expected<QueryPage, ProfilerError>    query(const QueryRequest& request) const   = 0;
        [[nodiscard]] virtual std::expected<DetailResult, ProfilerError> detail(const DetailRequest& request) const = 0;
        [[nodiscard]] virtual std::expected<HistoryPage, ProfilerError>
        history(const HistoryRequest& request) const                                                                = 0;
        [[nodiscard]] virtual std::expected<ExportResult, ProfilerError> exportReport(const ExportRequest& request) = 0;
        [[nodiscard]] virtual std::expected<CleanupResult, ProfilerError> cleanup(const CleanupRequest& request)    = 0;
        [[nodiscard]] virtual std::expected<Capabilities, ProfilerError>
        inspectCapabilities(const DoctorRequest& request) = 0;

        virtual void shutdown() noexcept = 0;
    };

} // namespace mcdk::performance
