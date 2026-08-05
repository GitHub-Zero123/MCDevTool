#include <mc_profiler_mcp.hpp>
#include <mcp_tool_definitions.hpp>
#include <performance/profiler_runtime_owner.hpp>
#include <performance/profiler_service_factory.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
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
        std::expected<QueryPage, ProfilerError> query(const QueryRequest&) const override {
            QueryRecord record{.id = "fake:0"};
            record.fields.emplace("total_time", ProfilerField{1.0, "seconds"});
            return QueryPage{
                .records = {std::move(record)},
                .totalAvailable = 2,
                .truncated = true,
                .nextCursor = "fake-cursor",
            };
        }
        std::expected<CompareResult, ProfilerError> compare(const CompareRequest&) const override {
            return CompareResult{};
        }
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
        passed &= expect(
            help && (*help)["structuredContent"]["data"].contains("initialization"),
            "help explains first-op service and Native DLL probing"
        );
        const auto startHelp = mcdk::mc_profiler_mcp::tryBuildLocalResult(
            {{"op", "/help"}, {"args", {{"topic", "/start"}}}}
        );
        passed &= expect(
            startHelp && (*startHelp)["structuredContent"]["data"]["storage"].value("default", "") == "memory",
            "start help explains temporary memory storage as the default"
        );
        passed &= expect(
            startHelp && (*startHelp)["structuredContent"]["data"]["optional"].size() == 6
                && (*startHelp)["structuredContent"]["data"].contains("bounds"),
            "start help lists all bounded optional fields"
        );
        const auto queryHelp = mcdk::mc_profiler_mcp::tryBuildLocalResult(
            {{"op", "/help"}, {"args", {{"topic", "/query"}}}}
        );
        passed &= expect(
            queryHelp && (*queryHelp)["structuredContent"]["data"].contains("views")
                && (*queryHelp)["structuredContent"]["data"].value("note", "").find("calltree-children")
                    != std::string::npos,
            "query help documents profiler views and call-tree parent filtering"
        );
        const auto cleanupHelp = mcdk::mc_profiler_mcp::tryBuildLocalResult(
            {{"op", "/help"}, {"args", {{"topic", "/cleanup"}}}}
        );
        passed &= expect(
            cleanupHelp && (*cleanupHelp)["structuredContent"]["data"].value("note", "").find("dry_run=true")
                    != std::string::npos,
            "cleanup help distinguishes retention cleanup from temporary lazy GC"
        );
        const auto compareHelp = mcdk::mc_profiler_mcp::tryBuildLocalResult(
            {{"op", "/help"}, {"args", {{"topic", "/compare"}}}}
        );
        passed &= expect(
            compareHelp && (*compareHelp)["structuredContent"]["data"].contains("supported_views"),
            "compare help documents stable cross-capture views"
        );
        const auto exportHelp = mcdk::mc_profiler_mcp::tryBuildLocalResult(
            {{"op", "/help"}, {"args", {{"topic", "/export"}}}}
        );
        passed &= expect(
            exportHelp && (*exportHelp)["structuredContent"]["data"]["formats"].size() == 2,
            "export help advertises Markdown and SVG"
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

        const auto oversizedOp =
            mcdk::mc_profiler_mcp::tryBuildLocalResult({{"op", std::string(65, 'x')}});
        passed &= expect(
            oversizedOp && (*oversizedOp)["structuredContent"]["error"]["code"] == "INVALID_ARGUMENTS",
            "oversized operation names are rejected without echoing unbounded input"
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

    bool testRuntimeHelpInitializesOnce() {
        std::atomic<int> factoryCount = 0;
        std::atomic<int> shutdownCount = 0;
        ProfilerRuntimeOwner owner([&]() -> std::expected<std::shared_ptr<ProfilerService>, ProfilerError> {
            ++factoryCount;
            return std::make_shared<FakeProfilerService>(shutdownCount);
        });

        const auto first = mcdk::mc_profiler_mcp::handleRuntimeRequest(owner.provider(), {{"op", "/help"}});
        const auto second = mcdk::mc_profiler_mcp::handleRuntimeRequest(owner.provider(), {{"op", "/guide"}});
        bool passed = true;
        passed &= expect(first["structuredContent"].value("ok", false), "runtime help succeeds");
        passed &= expect(
            first["structuredContent"]["data"].contains("runtime"),
            "runtime help contains dynamic capability information"
        );
        passed &= expect(second["structuredContent"].value("ok", false), "runtime guide succeeds");
        passed &= expect(factoryCount == 1, "first profiler op initializes the shared runtime exactly once");
        const auto page = mcdk::mc_profiler_mcp::handleRuntimeRequest(
            owner.provider(),
            {
                {"op", "/query"},
                {"args", {
                    {"job_id", "job"}, {"view", "hotspots"}, {"filter", "tick"},
                    {"sort", "self_time"}, {"order", "asc"}, {"limit", 7}
                }},
            }
        );
        const auto& nextArgs = page["structuredContent"]["next_calls"][0]["args"];
        passed &= expect(
            nextArgs.value("filter", "") == "tick" && nextArgs.value("sort", "") == "self_time"
                && nextArgs.value("order", "") == "asc" && nextArgs.value("limit", 0) == 7
                && nextArgs.value("cursor", "") == "fake-cursor",
            "query pagination preserves filter, ordering, limit, and the returned cursor"
        );
        const auto comparison = mcdk::mc_profiler_mcp::handleRuntimeRequest(
            owner.provider(),
            {
                {"op", "/compare"},
                {"args", {
                    {"baseline_job_id", "before"}, {"candidate_job_id", "after"},
                    {"view", "hotspots"}, {"metric", "self_time"}, {"limit", 5}
                }},
            }
        );
        passed &= expect(
            comparison["structuredContent"].value("ok", false)
                && comparison["structuredContent"]["data"].value("metric", "missing").empty(),
            "runtime adapter accepts bounded compare arguments"
        );
        return passed;
    }

    bool testBuiltNativeComponentProbe() {
#ifdef MCDEV_TEST_MCDK_DIR
        const auto root = std::filesystem::temp_directory_path()
                        / ("mcdev-profiler-native-probe-" + std::to_string(
                            std::chrono::steady_clock::now().time_since_epoch().count()
                        ));
        std::error_code ignored;
        auto service = createProfilerService({
            .executeCode = {},
            .currentGameProcessId = [] { return std::uint32_t{0}; },
            .storageRoot = root / "profiles",
            .executableDirectory = std::filesystem::path(MCDEV_TEST_MCDK_DIR),
        });
        bool passed = expect(service.has_value(), "service loads beside the built Native component");
        if (service) {
            auto capabilities = (*service)->inspectCapabilities(DoctorRequest{.kind = ProfilerKind::NativeCpu});
            passed &= expect(
                capabilities && capabilities->entries.size() == 1 && capabilities->entries.front().available,
                "fixed-path Native probe verifies manifest, hash, ABI, and Tracy protocol"
            );
            (*service)->shutdown();
        }
        std::filesystem::remove_all(root, ignored);
        return passed;
#else
        return true;
#endif
    }

    bool testAutomaticDeadlineAndPersistence() {
        const auto root = std::filesystem::temp_directory_path()
                        / ("mcdev-profiler-foundation-" + std::to_string(
                            std::chrono::steady_clock::now().time_since_epoch().count()
                        ));
        std::error_code ignored;
        std::filesystem::create_directories(root, ignored);

        std::atomic<int> startCalls = 0;
        std::atomic<int> collectCalls = 0;
        auto service = createProfilerService({
            .executeCode = [&](std::string code, ProfileTarget, std::chrono::milliseconds)
                -> std::expected<nlohmann::json, GameExecutionError> {
                if (code.find("_mcdev_pp_clock") != std::string::npos
                    && code.find("yappi.start") != std::string::npos) {
                    ++startCalls;
                    return nlohmann::json{{"ok", true}, {"running", true}, {"clock", "WALL"}};
                }
                if (code.find("_stats=yappi.get_func_stats()") != std::string::npos) {
                    ++collectCalls;
                    return nlohmann::json{
                        {"ok", true}, {"clock", "WALL"}, {"elapsed", 1.0}, {"total", 1},
                        {"truncated", false},
                        {"nodes", nlohmann::json::array({
                            nlohmann::json::array({0, "pack/foo.py", 12, "tick", 2, 2, 0.1, 0.3, 1, "Main", "client"})
                        })},
                        {"edges", nlohmann::json::array()},
                    };
                }
                return nlohmann::json(true);
            },
            .currentGameProcessId = [] { return std::uint32_t{0}; },
            .storageRoot = root / "profiles",
            .executableDirectory = root,
        });

        bool passed = true;
        passed &= expect(service.has_value(), "default profiler service is constructible without a Native component");
        if (!service) {
            std::filesystem::remove_all(root, ignored);
            return false;
        }
        auto started = (*service)->start(StartRequest{
            .storage = ProfileStorage::Disk,
            .duration = std::chrono::seconds(1),
        });
        passed &= expect(started.has_value(), "fake Python capture starts");
        if (!started) {
            (*service)->shutdown();
            std::filesystem::remove_all(root, ignored);
            return false;
        }
        auto prematureDetail = (*service)->detail(DetailRequest{started->id, "hotspots", "fn:0"});
        passed &= expect(
            !prematureDetail && prematureDetail.error().code == "JOB_NOT_QUERYABLE",
            "detail does not read a capture while its worker may still mutate data"
        );
        JobSnapshot snapshot;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        do {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            auto current = (*service)->status(started->id);
            if (current) snapshot = *current;
        } while (snapshot.state != JobState::Completed && snapshot.state != JobState::Failed
                 && std::chrono::steady_clock::now() < deadline);

        passed &= expect(snapshot.state == JobState::Completed, "server deadline completes capture without a stop call");
        passed &= expect(startCalls == 1 && collectCalls == 1, "deadline performs one start and one collect");
        passed &= expect(
            std::filesystem::is_regular_file(root / "profiles" / started->id / "manifest.json"),
            "manifest is committed after data and summary"
        );
        auto page = (*service)->query(QueryRequest{.jobId = started->id, .view = "hotspots", .limit = 20});
        passed &= expect(page && page->records.size() == 1, "completed bounded data is queryable through typed API");
        (*service)->shutdown();

        auto recoveredService = createProfilerService({
            .executeCode = {},
            .currentGameProcessId = [] { return std::uint32_t{0}; },
            .storageRoot = root / "profiles",
            .executableDirectory = root,
        });
        auto recoveredPage = recoveredService
            ? (*recoveredService)->query(QueryRequest{.jobId = started->id, .view = "hotspots", .limit = 20})
            : std::expected<QueryPage, ProfilerError>(std::unexpected(recoveredService.error()));
        passed &= expect(
            recoveredPage && recoveredPage->records.size() == 1,
            "a new service can recover and query a committed job"
        );
        auto recoveredHistory = recoveredService
            ? (*recoveredService)->history({})
            : std::expected<HistoryPage, ProfilerError>(std::unexpected(recoveredService.error()));
        passed &= expect(
            recoveredHistory && !recoveredHistory->jobs.empty()
                && recoveredHistory->jobs.front().storage == ProfileStorage::Disk,
            "history identifies recovered jobs as disk-backed"
        );
        auto report = recoveredService
            ? (*recoveredService)->exportReport(ExportRequest{started->id, ExportFormat::Markdown})
            : std::expected<ExportResult, ProfilerError>(std::unexpected(recoveredService.error()));
        passed &= expect(
            report && report->sha256.size() == 64 && std::filesystem::is_regular_file(report->path),
            "controlled exports include a SHA-256 digest"
        );
        if (recoveredService) (*recoveredService)->shutdown();
        std::filesystem::remove_all(root, ignored);
        return passed;
    }

    bool testTemporaryMemoryStorageAndLazyGc() {
        const auto root = std::filesystem::temp_directory_path()
                        / ("mcdev-profiler-memory-" + std::to_string(
                            std::chrono::steady_clock::now().time_since_epoch().count()
                        ));
        std::error_code ignored;
        std::atomic<std::int64_t> elapsedMilliseconds = 0;
        const auto clockOrigin = std::chrono::steady_clock::now();

        auto service = createProfilerService({
            .executeCode = [](std::string code, ProfileTarget, std::chrono::milliseconds)
                -> std::expected<nlohmann::json, GameExecutionError> {
                if (code.find("_mcdev_pp_clock") != std::string::npos
                    && code.find("yappi.start") != std::string::npos) {
                    return nlohmann::json{{"ok", true}, {"running", true}, {"clock", "WALL"}};
                }
                if (code.find("_stats=yappi.get_func_stats()") != std::string::npos) {
                    return nlohmann::json{
                        {"ok", true}, {"clock", "WALL"}, {"elapsed", 1.0}, {"total", 1},
                        {"truncated", false},
                        {"nodes", nlohmann::json::array({
                            nlohmann::json::array({0, "pack/memory.py", 7, "tick", 1, 1, 0.01, 0.02, 1, "Main", "client"})
                        })},
                        {"edges", nlohmann::json::array()},
                    };
                }
                return nlohmann::json(true);
            },
            .currentGameProcessId = [] { return std::uint32_t{0}; },
            .storageRoot = root / "profiles",
            .executableDirectory = root,
            .monotonicNow = [&] {
                return clockOrigin + std::chrono::milliseconds(elapsedMilliseconds.load());
            },
        });

        bool passed = expect(service.has_value(), "temporary memory service is constructible");
        if (!service) return false;
        auto started = (*service)->start(StartRequest{.duration = std::chrono::seconds(1)});
        passed &= expect(
            started && started->storage == ProfileStorage::Memory,
            "memory is the typed API default"
        );
        if (!started) {
            (*service)->shutdown();
            std::filesystem::remove_all(root, ignored);
            return false;
        }

        JobSnapshot snapshot;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        do {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            auto current = (*service)->status(started->id);
            if (current) snapshot = *current;
        } while (snapshot.state != JobState::Completed && snapshot.state != JobState::Failed
                 && std::chrono::steady_clock::now() < deadline);

        passed &= expect(snapshot.state == JobState::Completed, "temporary memory capture completes");
        passed &= expect(
            !std::filesystem::exists(root / "profiles" / started->id),
            "temporary memory result does not create a persistent job directory"
        );
        auto page = (*service)->query(QueryRequest{.jobId = started->id, .view = "hotspots", .limit = 20});
        passed &= expect(page && page->records.size() == 1, "temporary memory result remains queryable");
        auto history = (*service)->history({});
        passed &= expect(
            history && std::none_of(history->jobs.begin(), history->jobs.end(), [&](const auto& job) {
                return job.id == started->id;
            }),
            "temporary memory result is excluded from history"
        );

        const auto markdown = (*service)->exportReport({started->id, ExportFormat::Markdown});
        const auto svg = (*service)->exportReport({started->id, ExportFormat::Svg});
        passed &= expect(
            markdown && markdown->sha256.size() == 64 && std::filesystem::is_regular_file(markdown->path),
            "temporary memory result exports Markdown explicitly"
        );
        passed &= expect(
            svg && svg->sha256.size() == 64 && std::filesystem::is_regular_file(svg->path),
            "temporary memory result exports SVG explicitly"
        );
        if (markdown) {
            std::ifstream input(markdown->path, std::ios::binary);
            const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            passed &= expect(
                contents.find("## Ranked Records") != std::string::npos
                    && contents.find("pack/memory.py:7") != std::string::npos,
                "Markdown export contains a ranked profiler table"
            );
        }
        if (svg) {
            std::ifstream input(svg->path, std::ios::binary);
            const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
            passed &= expect(
                contents.find("Python Performance Profile") != std::string::npos
                    && contents.find(">Total</text>") != std::string::npos
                    && contents.find(">Self</text>") != std::string::npos
                    && contents.find("fill=\"#b27adf\"") != std::string::npos
                    && contents.find("20.000 ms / 10.000 ms") != std::string::npos,
                "SVG export visibly distinguishes total and self time"
            );
        }
        passed &= expect(
            !std::filesystem::is_regular_file(root / "profiles" / started->id / "manifest.json"),
            "explicit reports do not turn a memory job into a recoverable disk job"
        );

        elapsedMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::minutes(19)).count();
        auto refreshed = (*service)->status(started->id);
        passed &= expect(refreshed.has_value(), "job access before expiry refreshes the idle TTL");
        elapsedMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::minutes(38)).count();
        (void)(*service)->inspectCapabilities({});
        auto retained = (*service)->status(started->id);
        passed &= expect(retained.has_value(), "lazy GC retains a recently accessed memory job");
        elapsedMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::minutes(59)).count();
        (void)(*service)->inspectCapabilities({});
        auto expired = (*service)->status(started->id);
        passed &= expect(
            !expired && expired.error().code == "JOB_NOT_FOUND",
            "next profiler request lazily removes a memory job after 20 idle minutes"
        );

        auto discardCandidate = (*service)->start(StartRequest{.duration = std::chrono::seconds(1)});
        JobSnapshot discardSnapshot;
        const auto discardDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (discardCandidate && discardSnapshot.state != JobState::Completed
               && discardSnapshot.state != JobState::Failed
               && std::chrono::steady_clock::now() < discardDeadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            auto current = (*service)->status(discardCandidate->id);
            if (current) discardSnapshot = *current;
        }
        auto discardExport = discardCandidate
            ? (*service)->exportReport({discardCandidate->id, ExportFormat::Svg})
            : std::expected<ExportResult, ProfilerError>(std::unexpected(discardCandidate.error()));
        const auto discardExportDirectory = discardExport
            ? discardExport->path.parent_path() : std::filesystem::path{};
        auto discarded = discardCandidate
            ? (*service)->discard(discardCandidate->id)
            : std::expected<void, ProfilerError>(std::unexpected(discardCandidate.error()));
        passed &= expect(
            discarded && !discardExportDirectory.empty() && !std::filesystem::exists(discardExportDirectory),
            "discard removes explicit exports belonging to a temporary memory job"
        );

        (*service)->shutdown();
        std::filesystem::remove_all(root, ignored);
        return passed;
    }

    bool testNativeDoesNotFallbackWithoutDll() {
        const auto root = std::filesystem::temp_directory_path()
                        / ("mcdev-profiler-no-native-" + std::to_string(
                            std::chrono::steady_clock::now().time_since_epoch().count()
                        ));
        auto service = createProfilerService({
            .executeCode = [](std::string, ProfileTarget, std::chrono::milliseconds)
                -> std::expected<nlohmann::json, GameExecutionError> {
                return nlohmann::json{{"ok", true}};
            },
            .currentGameProcessId = [] { return std::uint32_t{123}; },
            .storageRoot = root / "profiles",
            .executableDirectory = root,
        });
        bool passed = expect(service.has_value(), "service remains available when optional Native DLL is absent");
        if (service) {
            auto native = (*service)->start(StartRequest{
                .kind = ProfilerKind::NativeCpu,
                .duration = std::chrono::seconds(1),
            });
            passed &= expect(
                !native && native.error().code == "NATIVE_COMPONENT_MISSING",
                "native capture fails instead of falling back when its sibling DLL is absent"
            );
            (*service)->shutdown();
        }
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        return passed;
    }

    bool testPythonCaptureOwnershipTokens() {
        const auto root = std::filesystem::temp_directory_path()
                        / ("mcdev-profiler-owner-" + std::to_string(
                            std::chrono::steady_clock::now().time_since_epoch().count()
                        ));
        std::mutex codesMutex;
        std::vector<std::string> codes;
        auto service = createProfilerService({
            .executeCode = [&](std::string code, ProfileTarget, std::chrono::milliseconds)
                -> std::expected<nlohmann::json, GameExecutionError> {
                {
                    std::lock_guard lock(codesMutex);
                    codes.push_back(code);
                }
                if (code.find("yappi.start") != std::string::npos) {
                    return nlohmann::json{{"ok", true}, {"running", true}, {"clock", "WALL"}};
                }
                if (code.find("tracemalloc.start") != std::string::npos) {
                    return nlohmann::json{{"ok", true}, {"depth", 8}};
                }
                if (code.find("yappi.get_func_stats") != std::string::npos) {
                    return nlohmann::json{
                        {"ok", true}, {"clock", "WALL"}, {"elapsed", 1.0}, {"total", 0},
                        {"truncated", false}, {"targets", nlohmann::json::array()},
                        {"nodes", nlohmann::json::array()}, {"edges", nlohmann::json::array()},
                    };
                }
                return nlohmann::json(true);
            },
            .currentGameProcessId = [] { return std::uint32_t{0}; },
            .storageRoot = root / "profiles",
            .executableDirectory = root,
        });
        bool passed = expect(service.has_value(), "ownership-token service is constructible");
        if (!service) return false;

        const auto discardAndWait = [&](ProfilerKind kind) -> std::optional<JobSnapshot> {
            auto started = (*service)->start(StartRequest{.kind = kind, .duration = std::chrono::seconds(10)});
            if (!started) return std::nullopt;
            (void)(*service)->discard(started->id);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
            while (std::chrono::steady_clock::now() < deadline) {
                auto current = (*service)->status(started->id);
                if (current && current->state == JobState::Discarded) return *current;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            return std::nullopt;
        };

        const auto cpu = discardAndWait(ProfilerKind::PythonCpu);
        const auto memory = discardAndWait(ProfilerKind::PythonMemory);
        passed &= expect(cpu.has_value(), "discarded Python CPU capture completes cleanup");
        passed &= expect(memory.has_value(), "discarded Python memory capture completes cleanup");

        const auto containsCode = [&](std::string_view prefix, std::string_view required) {
            std::lock_guard lock(codesMutex);
            return std::any_of(codes.begin(), codes.end(), [&](const std::string& code) {
                return code.starts_with(prefix) && code.find(required) != std::string::npos;
            });
        };
        if (cpu) {
            const auto token = "_mcdev_pp_owner='" + cpu->id + "'";
            passed &= expect(
                containsCode("import yappi,threading,time", token)
                    && containsCode("import yappi\n", token),
                "Python CPU start and cleanup share one job owner token"
            );
        }
        if (memory) {
            const auto token = "_mcdev_pm_owner='" + memory->id + "'";
            passed &= expect(
                containsCode("import tracemalloc,time,threading", token)
                    && containsCode("import tracemalloc\n", token),
                "Python memory start and cleanup share one job owner token"
            );
        }
        passed &= expect(
            containsCode("import yappi,threading,time", "if yappi.is_running() or globals().get('_mcdev_pp_owned',False):")
                && containsCode("import tracemalloc,time,threading", "if tracemalloc.is_tracing() or globals().get('_mcdev_pm_owned',False):"),
            "MCP refuses to steal a Python profiler already owned by another frontend"
        );

        auto allCpu = (*service)->start(StartRequest{
            .kind = ProfilerKind::PythonCpu,
            .target = ProfileTarget::All,
            .duration = std::chrono::seconds(1),
        });
        if (allCpu) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
            while (std::chrono::steady_clock::now() < deadline) {
                const auto current = (*service)->status(allCpu->id);
                if (current && current->state == JobState::Completed) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        passed &= expect(allCpu.has_value(), "all-target Python CPU capture starts");
        passed &= expect(
            containsCode(
                "import yappi,time",
                "_sides.get(_parent.index)==_sides.get(_child.index)"
            ),
            "all-target Python CPU call edges cannot cross client/server contexts"
        );

        (*service)->shutdown();
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        return passed;
    }

    bool testNativeCalltreeChildrenUseExactParentId() {
        const auto root = std::filesystem::temp_directory_path()
                        / ("mcdev-profiler-calltree-" + std::to_string(
                            std::chrono::steady_clock::now().time_since_epoch().count()
                        ));
        const auto profiles = root / "profiles";
        const auto jobId = std::string("native-calltree-job");
        const auto jobDirectory = profiles / jobId;
        std::error_code ignored;
        std::filesystem::create_directories(jobDirectory, ignored);

        const auto node = [](std::int64_t id, nlohmann::json children = nlohmann::json::array()) {
            return nlohmann::json{
                {"id", id}, {"name", "zone-" + std::to_string(id)}, {"sourceFile", "Engine"},
                {"sourceLine", 0}, {"calls", 1}, {"totalNanoseconds", 100}, {"selfNanoseconds", 50},
                {"meanNanoseconds", 100}, {"maximumNanoseconds", 100}, {"children", std::move(children)},
            };
        };
        const nlohmann::json data{
            {"threads", nlohmann::json::array({
                nlohmann::json{
                    {"id", "1"}, {"name", "Main"},
                    {"roots", nlohmann::json::array({
                        node(39, nlohmann::json::array({node(40)})),
                        node(390, nlohmann::json::array({node(392)})),
                    })},
                },
            })},
            {"zones", nlohmann::json::array({
                nlohmann::json{
                    {"id", 0}, {"name", "parse"}, {"sourceFile", "engine/json.cpp"}, {"sourceLine", 8},
                    {"threadId", "1"}, {"threadName", "Main"}, {"calls", 1},
                    {"totalNanoseconds", 100}, {"selfNanoseconds", 40},
                    {"meanNanoseconds", 100}, {"maximumNanoseconds", 100},
                    {"slowestCalls", nlohmann::json::array({
                        nlohmann::json{{"startNanoseconds", 10}, {"durationNanoseconds", 100}}
                    })},
                },
                nlohmann::json{
                    {"id", 1}, {"name", "parse"}, {"sourceFile", "engine/json.cpp"}, {"sourceLine", 8},
                    {"threadId", "2"}, {"threadName", "Worker"}, {"calls", 2},
                    {"totalNanoseconds", 200}, {"selfNanoseconds", 60},
                    {"meanNanoseconds", 100}, {"maximumNanoseconds", 150},
                    {"slowestCalls", nlohmann::json::array({
                        nlohmann::json{{"startNanoseconds", 20}, {"durationNanoseconds", 150}}
                    })},
                },
            })},
        };
        const nlohmann::json manifest{
            {"job_id", jobId}, {"kind", "native.cpu"}, {"state", "completed"},
            {"partial", false}, {"created_at", "2026-01-01T00:00:00Z"},
            {"completed_at", "2026-01-01T00:00:01Z"},
        };
        {
            std::ofstream(jobDirectory / "manifest.json") << manifest.dump();
            std::ofstream(jobDirectory / "data.json") << data.dump();
            std::ofstream(jobDirectory / "summary.json") << nlohmann::json::object().dump();
        }

        auto service = createProfilerService({
            .executeCode = {},
            .currentGameProcessId = [] { return std::uint32_t{0}; },
            .storageRoot = profiles,
            .executableDirectory = root,
        });
        bool passed = expect(service.has_value(), "native calltree fixture service is constructible");
        if (service) {
            auto missingParent = (*service)->query(QueryRequest{
                .jobId = jobId,
                .view = "calltree-children",
                .limit = 20,
            });
            passed &= expect(
                !missingParent && missingParent.error().code == "CALLTREE_PARENT_REQUIRED",
                "calltree child queries require an explicit parent node id"
            );
            auto children = (*service)->query(QueryRequest{
                .jobId = jobId,
                .view = "calltree-children",
                .filter = "node:39",
                .limit = 20,
            });
            passed &= expect(
                children && children->records.size() == 1 && children->records.front().id == "node:40",
                "calltree child selection does not prefix-match unrelated node ids"
            );
            auto rootDetail = (*service)->detail(DetailRequest{
                .jobId = jobId,
                .view = "calltree-roots",
                .recordId = "node:39",
            });
            passed &= expect(
                rootDetail && rootDetail->related.size() == 1 && rootDetail->related.front().id == "node:40",
                "calltree root detail returns its children without unrelated roots"
            );
            auto sources = (*service)->query(QueryRequest{
                .jobId = jobId,
                .view = "source-locations",
                .limit = 20,
            });
            passed &= expect(
                sources && sources->records.size() == 1
                    && std::get<std::int64_t>(sources->records[0].fields.at("total_time").value) == 300
                    && std::get<std::int64_t>(sources->records[0].fields.at("self_time").value) == 100
                    && std::get<std::int64_t>(sources->records[0].fields.at("thread_count").value) == 2,
                "Native source-locations view aggregates the same source across threads"
            );
            auto slowest = (*service)->query(QueryRequest{
                .jobId = jobId,
                .view = "slowest-calls",
                .limit = 20,
            });
            passed &= expect(
                slowest && slowest->records.size() == 2
                    && std::get<std::int64_t>(slowest->records[0].fields.at("duration").value) == 150,
                "Native slowest-calls exposes bounded event-level start and duration samples"
            );
            (*service)->shutdown();
        }
        std::filesystem::remove_all(root, ignored);
        return passed;
    }

    bool testSemanticViewsStructuredTracebackAndCompare() {
        const auto root = std::filesystem::temp_directory_path()
                        / ("mcdev-profiler-semantic-" + std::to_string(
                            std::chrono::steady_clock::now().time_since_epoch().count()
                        ));
        const auto profiles = root / "profiles";
        std::error_code ignored;
        const auto writeJob = [&](std::string_view id, std::string_view kind, const nlohmann::json& data) {
            const auto directory = profiles / id;
            std::filesystem::create_directories(directory, ignored);
            const nlohmann::json manifest{
                {"job_id", id}, {"kind", kind}, {"state", "completed"}, {"partial", false},
                {"created_at", "2026-01-01T00:00:00Z"}, {"completed_at", "2026-01-01T00:00:01Z"},
            };
            std::ofstream(directory / "manifest.json") << manifest.dump();
            std::ofstream(directory / "data.json") << data.dump();
            std::ofstream(directory / "summary.json") << nlohmann::json::object().dump();
        };
        const auto cpuNode = [](std::int64_t id, std::string name, double self, double total) {
            return nlohmann::json::array({
                id, "pack/perf.py", 12, std::move(name), 10, 10, self, total, 1, "Main", "client"
            });
        };
        writeJob(
            "baseline", "python.cpu",
            nlohmann::json{
                {"nodes", nlohmann::json::array({cpuNode(7, "tick", 0.04, 0.10), cpuNode(8, "removed", 0.01, 0.02)})},
                {"edges", nlohmann::json::array({nlohmann::json::array({7, 8, 2, 0.01, 0.02})})},
            }
        );
        writeJob(
            "candidate", "python.cpu",
            nlohmann::json{
                {"nodes", nlohmann::json::array({cpuNode(3, "tick", 0.07, 0.16), cpuNode(4, "added", 0.02, 0.03)})},
                {"edges", nlohmann::json::array()},
            }
        );
        writeJob(
            "memory", "python.memory",
            nlohmann::json{
                {"rows", nlohmann::json::array({
                    nlohmann::json::array({0, 100, 2, 300, 4, nlohmann::json::array({
                        nlohmann::json::array({"pack/a.py", 7}), nlohmann::json::array({"pack/b.py", 9})
                    })}),
                    nlohmann::json::array({1, -50, -1, 25, 1, nlohmann::json::array({
                        nlohmann::json::array({"pack/c.py", 11})
                    })}),
                })},
            }
        );

        auto service = createProfilerService({
            .executeCode = {},
            .currentGameProcessId = [] { return std::uint32_t{0}; },
            .storageRoot = profiles,
            .executableDirectory = root,
        });
        bool passed = expect(service.has_value(), "semantic view fixture service is constructible");
        if (service) {
            auto growth = (*service)->query(QueryRequest{.jobId = "memory", .view = "growth", .limit = 20});
            passed &= expect(
                growth && growth->records.size() == 2
                    && std::holds_alternative<ProfilerStackTrace>(growth->records[0].fields.at("traceback").value)
                    && std::get<ProfilerStackTrace>(growth->records[0].fields.at("traceback").value).size() == 2,
                "memory growth exposes traceback as structured frames"
            );
            auto retained = (*service)->query(QueryRequest{.jobId = "memory", .view = "retained", .limit = 20});
            passed &= expect(
                retained && retained->records.size() == 2
                    && !retained->records[0].fields.contains("size_diff")
                    && retained->records[0].fields.contains("current_size"),
                "memory retained view has distinct current-retention semantics"
            );
            auto fakeView = (*service)->query(QueryRequest{.jobId = "baseline", .view = "functions", .limit = 20});
            passed &= expect(
                !fakeView && fakeView.error().code == "VIEW_INVALID",
                "legacy alias views no longer pretend to provide distinct data"
            );
            auto calls = (*service)->query(QueryRequest{.jobId = "baseline", .view = "calls", .limit = 20});
            passed &= expect(
                calls && calls->records.size() == 1
                    && std::get<std::string>(calls->records[0].fields.at("caller_name").value) == "tick"
                    && std::get<std::string>(calls->records[0].fields.at("callee_name").value) == "removed",
                "Python calls view retains node storage while resolving call-edge names"
            );
            auto compared = (*service)->compare(CompareRequest{
                .baselineJobId = "baseline",
                .candidateJobId = "candidate",
                .view = "hotspots",
                .limit = 20,
            });
            const QueryRecord* tick = nullptr;
            if (compared) {
                for (const auto& record : compared->records) {
                    const auto found = record.fields.find("name");
                    if (found != record.fields.end() && std::get<std::string>(found->second.value) == "tick") {
                        tick = &record;
                    }
                }
            }
            passed &= expect(
                compared && compared->matched == 1 && compared->added == 1 && compared->removed == 1
                    && compared->metric == "total_time" && tick
                    && std::abs(std::get<double>(tick->fields.at("delta").value) - 0.06) < 0.000001,
                "compare aligns stable function identities and reports bounded deltas"
            );
            auto invalidMetric = (*service)->compare(CompareRequest{
                .baselineJobId = "baseline", .candidateJobId = "candidate",
                .view = "hotspots", .metric = "line",
            });
            passed &= expect(
                !invalidMetric && invalidMetric.error().code == "COMPARE_METRIC_INVALID",
                "compare rejects numeric identity fields as performance metrics"
            );
            auto mismatch = (*service)->compare(CompareRequest{
                .baselineJobId = "baseline", .candidateJobId = "memory", .view = "hotspots",
            });
            passed &= expect(
                !mismatch && mismatch.error().code == "COMPARE_KIND_MISMATCH",
                "compare rejects jobs of different profiler kinds"
            );
            (*service)->shutdown();
        }
        std::filesystem::remove_all(root, ignored);
        return passed;
    }

} // namespace

int main() {
    bool passed  = true;
    passed      &= testLazyProvider();
    passed      &= testRetryableFactory();
    passed      &= testToolContract();
    passed      &= testRuntimeHelpInitializesOnce();
    passed      &= testBuiltNativeComponentProbe();
    passed      &= testAutomaticDeadlineAndPersistence();
    passed      &= testTemporaryMemoryStorageAndLazyGc();
    passed      &= testPythonCaptureOwnershipTokens();
    passed      &= testNativeDoesNotFallbackWithoutDll();
    passed      &= testNativeCalltreeChildrenUseExactParentId();
    passed      &= testSemanticViewsStructuredTracebackAndCompare();
    return passed ? 0 : 1;
}
