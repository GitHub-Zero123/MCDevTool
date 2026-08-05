#include <mc_profiler_mcp.hpp>
#include <mcp_tool_definitions.hpp>
#include <performance/profiler_runtime_owner.hpp>

#include <atomic>
#include <expected>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

    using namespace mcdk::performance;

    bool expect(bool condition, const char* description) {
        if (!condition) {
            std::cerr << "Failed: " << description << '\n';
        }
        return condition;
    }

    class FakeProfilerService final : public ProfilerService {
    public:
        explicit FakeProfilerService(std::atomic<int>& shutdownCount) : shutdownCount_(shutdownCount) {}

        std::expected<JobSnapshot, ProfilerError>  start(const StartRequest&) override { return JobSnapshot{}; }
        std::expected<JobSnapshot, ProfilerError>  status(const JobId&) const override { return JobSnapshot{}; }
        std::expected<JobSnapshot, ProfilerError>  stop(const JobId&) override { return JobSnapshot{}; }
        std::expected<void, ProfilerError>         discard(const JobId&) override { return {}; }
        std::expected<QueryPage, ProfilerError>    query(const QueryRequest&) const override { return QueryPage{}; }
        std::expected<DetailResult, ProfilerError> detail(const DetailRequest&) const override {
            return DetailResult{};
        }
        std::expected<HistoryPage, ProfilerError> history(const HistoryRequest&) const override {
            return HistoryPage{};
        }
        std::expected<ExportResult, ProfilerError> exportReport(const ExportRequest&) override {
            return ExportResult{};
        }
        std::expected<CleanupResult, ProfilerError> cleanup(const CleanupRequest&) override { return CleanupResult{}; }
        std::expected<Capabilities, ProfilerError>  inspectCapabilities(const DoctorRequest&) override {
            return Capabilities{};
        }
        void shutdown() noexcept override {
            if (!stopped_.exchange(true)) {
                ++shutdownCount_;
            }
        }

    private:
        std::atomic<int>& shutdownCount_;
        std::atomic<bool> stopped_ = false;
    };

    bool testLazyProvider() {
        std::atomic<int>     factoryCount  = 0;
        std::atomic<int>     shutdownCount = 0;
        ProfilerRuntimeOwner owner([&]() -> std::expected<std::shared_ptr<ProfilerService>, ProfilerError> {
            ++factoryCount;
            return std::make_shared<FakeProfilerService>(shutdownCount);
        });

        bool passed  = true;
        passed      &= expect(!owner.provider().initialized(), "provider starts uninitialized");
        passed      &= expect(factoryCount == 0, "owner construction does not invoke the factory");

        std::vector<std::shared_ptr<ProfilerService>> services(12);
        std::vector<std::thread>                      threads;
        threads.reserve(services.size());
        for (std::size_t index = 0; index < services.size(); ++index) {
            threads.emplace_back([&, index] {
                auto service = owner.provider().get();
                if (service) {
                    services[index] = *service;
                }
            });
        }
        for (auto& thread : threads) {
            thread.join();
        }

        passed &= expect(factoryCount == 1, "concurrent first access invokes one factory");
        passed &= expect(owner.provider().initialized(), "provider publishes the initialized service");
        for (const auto& service : services) {
            passed &= expect(service && service == services.front(), "all callers share one service instance");
        }

        owner.shutdown();
        passed &= expect(shutdownCount == 1, "owner shutdown stops the service exactly once");
        passed &= expect(owner.provider().stopped(), "provider records terminal shutdown");
        passed &= expect(!owner.provider().get(), "shutdown provider rejects later access");
        owner.shutdown();
        passed &= expect(shutdownCount == 1, "owner shutdown is idempotent");
        return passed;
    }

    bool testRetryableFactory() {
        int                  attempts      = 0;
        std::atomic<int>     shutdownCount = 0;
        ProfilerRuntimeOwner owner([&]() -> std::expected<std::shared_ptr<ProfilerService>, ProfilerError> {
            ++attempts;
            if (attempts == 1) {
                return std::unexpected(
                    ProfilerError{
                        .code      = "TEMPORARY_FAILURE",
                        .message   = "retry",
                        .retryable = true,
                    }
                );
            }
            return std::make_shared<FakeProfilerService>(shutdownCount);
        });

        const auto first  = owner.provider().get();
        const auto second = owner.provider().get();
        bool       passed = true;
        passed &= expect(!first && first.error().retryable, "factory failure is returned without publication");
        passed &= expect(second.has_value(), "provider retries factory after a failed initialization");
        passed &= expect(attempts == 2, "retry performs one additional factory call");
        return passed;
    }

    bool testToolContract() {
        const auto tool    = mcdk::mcp_tool_definitions::buildMcProfilerTool();
        bool       passed  = true;
        passed            &= expect(tool.name == "mc_profiler", "tool uses the stable public name");
        passed &= expect(tool.parameters_schema.value("additionalProperties", true) == false, "input is closed");
        passed &= expect(tool.output_schema.value("additionalProperties", true) == false, "output is closed");
        passed &= expect(tool.annotations.read_only_hint == false, "single tool is conservatively non-read-only");
        passed &= expect(tool.annotations.destructive_hint == true, "single tool is conservatively destructive");
        passed &= expect(tool.annotations.idempotent_hint == false, "single tool is non-idempotent");
        passed &= expect(tool.annotations.open_world_hint == true, "single tool interacts with game state");

        int profilerToolCount = 0;
        for (const auto& candidate : mcdk::mcp_tool_definitions::buildAllTools()) {
            if (candidate.name == "mc_profiler") {
                ++profilerToolCount;
            }
        }
        passed &= expect(profilerToolCount == 1, "buildAllTools exposes exactly one profiler tool");

        const auto help  = mcdk::mc_profiler_mcp::tryBuildLocalResult({{"op", "/help"}});
        passed          &= expect(help.has_value(), "help is handled locally");
        passed          &= expect(
            help && (*help)["structuredContent"].value("ok", false),
            "help returns a successful structured envelope"
        );

        const auto guide  = mcdk::mc_profiler_mcp::tryBuildLocalResult({{"op", "/guide"}});
        passed           &= expect(guide.has_value(), "guide is handled locally");
        passed           &= expect(
            guide && (*guide)["structuredContent"]["data"]["guides"][0] == "lag",
            "guide catalog order is deterministic"
        );

        const auto nativeHelp =
            mcdk::mc_profiler_mcp::tryBuildLocalResult({{"op", "/help"}, {"args", {{"topic", "native.cpu"}}}});
        passed &= expect(
            nativeHelp && (*nativeHelp)["structuredContent"]["data"].contains("analysis_model")
                && (*nativeHelp)["structuredContent"]["data"].contains("engine_stage_examples")
                && (*nativeHelp)["structuredContent"]["data"].contains("analysis_strategy")
                && (*nativeHelp)["structuredContent"]["data"].contains("limitations"),
            "native help explains cross-language evidence, engine stages, analysis, and limitations"
        );

        const auto nativeGuide =
            mcdk::mc_profiler_mcp::tryBuildLocalResult({{"op", "/guide"}, {"args", {{"name", "native-hotspot"}}}});
        passed &= expect(
            nativeGuide && (*nativeGuide)["structuredContent"]["data"].contains("interpretation")
                && (*nativeGuide)["structuredContent"]["data"]["steps"].size() >= 6,
            "native hotspot guide drives thread and call-tree analysis"
        );

        passed &= expect(
            !mcdk::mc_profiler_mcp::tryBuildLocalResult(
                {{"op", "/start"}, {"args", {{"kind", "python.cpu"}, {"duration_seconds", 15}}}}
            ),
            "runtime start is not consumed by the local help dispatcher"
        );

        const auto unknown  = mcdk::mc_profiler_mcp::tryBuildLocalResult({{"op", "/made-up"}});
        passed             &= expect(
            unknown && (*unknown)["structuredContent"]["error"]["code"] == "UNKNOWN_OPERATION",
            "unknown operations are rejected locally"
        );

        const auto extra  = mcdk::mc_profiler_mcp::tryBuildLocalResult({{"op", "/help"}, {"extra", true}});
        passed           &= expect(
            extra && (*extra)["structuredContent"]["error"]["code"] == "INVALID_ARGUMENTS",
            "unknown top-level fields are rejected"
        );

        std::string validationError;
        auto        invalidEnvelope = (*help)["structuredContent"];
        invalidEnvelope["next_calls"].push_back(nlohmann::json::object());
        invalidEnvelope["next_calls"].push_back(nlohmann::json::object());
        passed &= expect(
            !mcdk::mc_profiler_mcp::validateProfilerEnvelope(invalidEnvelope, validationError),
            "envelope validator enforces the next-call budget"
        );
        return passed;
    }

} // namespace

int main() {
    bool passed  = true;
    passed      &= testLazyProvider();
    passed      &= testRetryableFactory();
    passed      &= testToolContract();
    return passed ? 0 : 1;
}
