#include <mc_profiler_mcp.hpp>

#include <mcp_tool_definitions.hpp>

#include <array>
#include <algorithm>
#include <charconv>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <utility>

namespace {

    using Json = nlohmann::json;

    constexpr std::array<std::string_view, 12> SupportedOperations = {
        "/help",
        "/guide",
        "/doctor",
        "/start",
        "/status",
        "/stop",
        "/query",
        "/detail",
        "/history",
        "/export",
        "/discard",
        "/cleanup",
    };

    bool isSupportedOperation(std::string_view op) {
        for (const auto candidate : SupportedOperations) {
            if (candidate == op) {
                return true;
            }
        }
        return false;
    }

    bool hasOnlyFields(const Json& value, std::initializer_list<std::string_view> allowed) {
        if (!value.is_object()) {
            return false;
        }
        for (const auto& [key, ignored] : value.items()) {
            (void)ignored;
            bool found = false;
            for (const auto field : allowed) {
                if (key == field) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                return false;
            }
        }
        return true;
    }

    Json nextCall(std::string_view op, Json args, std::string_view reason) {
        return Json{{"op", op}, {"args", std::move(args)}, {"reason", reason}};
    }

    Json makeEnvelope(bool ok, std::string_view op, Json data, Json error, Json warnings, Json nextCalls) {
        return Json{
            {"ok", ok},
            {"op", op},
            {"job", nullptr},
            {"data", std::move(data)},
            {"error", std::move(error)},
            {"warnings", std::move(warnings)},
            {"next_calls", std::move(nextCalls)},
        };
    }

    Json makeToolResult(Json envelope, std::string summary) {
        std::string validationError;
        if (!mcdk::mc_profiler_mcp::validateProfilerEnvelope(envelope, validationError)) {
            envelope = makeEnvelope(
                false,
                envelope.value("op", ""),
                nullptr,
                Json{
                    {"code", "PROFILER_ENVELOPE_INVALID"},
                    {"message", validationError},
                    {"retryable", false},
                    {"details", Json::object()},
                },
                Json::array(),
                Json::array()
            );
            summary = "mc_profiler produced an invalid response envelope.";
        }
        return Json{
            {"isError", !envelope.at("ok").get<bool>()},
            {"content", Json::array({Json{{"type", "text"}, {"text", std::move(summary)}}})},
            {"structuredContent", std::move(envelope)},
        };
    }

    Json staticArgumentError(std::string_view op, std::string_view message) {
        return mcdk::mc_profiler_mcp::buildErrorResult(op, "INVALID_ARGUMENTS", message, false);
    }

    Json buildHelp(std::string_view topic) {
        Json data = {
            {"tool", "mc_profiler"},
            {"invocation", Json{{"op", "/..."}, {"args", Json::object()}}},
            {"safety",
             Json::array(
                 {"Every capture has a server-enforced deadline.",
                  "Query results are filtered, paged, and byte-bounded by the server.",
                  "Artifact paths and retention are controlled by the server.",
                  "Only one profiler job is active by default."}
             )},
            {"initialization",
             "The first mc_profiler operation initializes the shared service and probes the fixed Native DLL path so "
             "runtime help can report capability. Tracy endpoint discovery remains deferred until Native start or a "
             "deep Native doctor."},
        };

        Json nextCalls = Json::array();
        if (topic.empty()) {
            data["operations"] = SupportedOperations;
            data["kinds"]      = Json::array({"python.cpu", "python.memory", "native.cpu"});
            data["note"]       = "Use /help with args.topic set to an operation or profiler kind for focused help.";
            nextCalls.push_back(nextCall("/guide", Json{{"name", "lag"}}, "Choose a bounded diagnostic workflow."));
            nextCalls.push_back(nextCall("/doctor", Json::object(), "Inspect static runtime capabilities."));
        } else if (topic == "/start") {
            data["topic"]       = "/start";
            data["required"]    = Json::array({"kind"});
            data["common_args"] = Json::array({"target", "clock", "duration_seconds"});
            data["example"]     = Json{
                    {"op", "/start"},
                    {"args", {{"kind", "python.cpu"}, {"target", "client"}, {"clock", "wall"}, {"duration_seconds", 15}}},
            };
            data["note"] = "The backend requests stop at the deadline. Native finalization may report cleanup_pending.";
            nextCalls.push_back(nextCall("/doctor", Json::object(), "Check availability before starting."));
        } else if (topic == "python.cpu" || topic == "python.memory" || topic == "native.cpu") {
            data["topic"] = topic;
            data["kind"]  = topic;
            if (topic == "native.cpu") {
                data["note"] =
                    "Native profiling is Windows x64 only and lazily discovers a Tracy endpoint owned by the current "
                    "game PID.";
                data["analysis_model"] =
                    "A per-thread Tracy zone hierarchy can correlate instrumented Python-facing script work with "
                    "engine C++ work. It is a zone call hierarchy, not an unconditional sampled machine stack.";
                data["available_evidence"] = Json::array(
                    {"Cross-language parent/child zone relationships when both Python and C++ paths emit Tracy zones.",
                     "Zone and thread names plus source file and line when supplied by instrumentation.",
                     "Calls, inclusive total time, self time, mean time, and maximum time.",
                     "A complete .tracy artifact plus a bounded query index with explicit truncation and coverage."}
                );
                data["engine_stage_examples"] = Json::array(
                    {"Data-driven JSON parsing and deserialization.",
                     "Schema or property conversion and validation.",
                     "Engine object construction and resource loading.",
                     "Event dispatch, script/native crossings, and downstream C++ work."}
                );
                data["analysis_strategy"] = Json::array(
                    {"Find the affected thread and slow interval before ranking global hotspots.",
                     "Follow calltree-roots and calltree-children across Python/C++ boundaries to locate the first "
                     "slow child.",
                     "High total with low self points to descendants; high self points to work inside that zone.",
                     "Use calls, mean, and maximum together to separate repeated overhead from isolated stalls.",
                     "For data-driven JSON, compare parse, conversion, construction, and dispatch descendants instead "
                     "of blaming the outer load zone."}
                );
                data["recommended_views"] =
                    Json::array({"threads", "calltree-roots", "calltree-children", "hotspots", "source-locations"});
                data["limitations"] = Json::array(
                    {"Uninstrumented native work and arbitrary OS stack frames may not appear in the zone hierarchy.",
                     "Python/C++ correlation depends on the game emitting nested zones on the observed path.",
                     "Zone names and source locations depend on the target build and available instrumentation.",
                     "The bounded index may be truncated; inspect coverage fields and use the .tracy artifact as the "
                     "source of truth."}
                );
            } else {
                data["note"] =
                    "The first mc_profiler operation probes the optional Native DLL so help can report capability. "
                    "Python capture itself uses game IPC and never performs Tracy endpoint discovery.";
            }
            nextCalls.push_back(nextCall("/doctor", Json{{"kind", topic}}, "Check this profiler kind."));
            nextCalls.push_back(nextCall("/help", Json{{"topic", "/start"}}, "Review bounded start parameters."));
        } else if (isSupportedOperation(topic)) {
            data["topic"] = topic;
            data["note"]  = "Operation-specific runtime fields are strictly validated by the backend adapter.";
        } else {
            return mcdk::mc_profiler_mcp::buildErrorResult(
                "/help",
                "HELP_TOPIC_NOT_FOUND",
                "Unknown help topic. Use /help without args to list operations and profiler kinds.",
                false
            );
        }

        return makeToolResult(
            makeEnvelope(true, "/help", std::move(data), nullptr, Json::array(), std::move(nextCalls)),
            "mc_profiler help is available in structuredContent."
        );
    }

    Json buildGuide(std::string_view name) {
        static constexpr std::array<std::string_view, 5> GuideNames = {
            "lag",
            "python-hotspot",
            "memory-growth",
            "native-hotspot",
            "compare-before-after",
        };

        bool knownGuide = false;
        for (const auto candidate : GuideNames) {
            if (candidate == name) {
                knownGuide = true;
                break;
            }
        }

        if (name.empty()) {
            Json guides = Json::array();
            for (const auto guideName : GuideNames) {
                guides.push_back(guideName);
            }
            return makeToolResult(
                makeEnvelope(
                    true,
                    "/guide",
                    Json{{"guides", std::move(guides)}},
                    nullptr,
                    Json::array(),
                    Json::array({nextCall("/guide", Json{{"name", "lag"}}, "Start from symptom-driven triage.")})
                ),
                "mc_profiler guides are available in structuredContent."
            );
        }
        if (!knownGuide) {
            return mcdk::mc_profiler_mcp::buildErrorResult(
                "/guide",
                "GUIDE_NOT_FOUND",
                "Unknown guide. Use /guide without args to list available guides.",
                false
            );
        }

        Json steps = Json::array();
        if (name == "lag") {
            steps.push_back(Json{{"op", "/doctor"}, {"args", Json::object()}});
            steps.push_back(Json{{"op", "/start"}, {"args", {{"kind", "python.cpu"}, {"duration_seconds", 15}}}});
            steps.push_back(Json{{"op", "/query"}, {"args", {{"job_id", "$start.job.id"}, {"view", "hotspots"}, {"limit", 20}}}});
        } else if (name == "memory-growth") {
            steps.push_back(Json{{"op", "/start"}, {"args", {{"kind", "python.memory"}, {"duration_seconds", 20}}}});
            steps.push_back(Json{{"op", "/query"}, {"args", {{"job_id", "$start.job.id"}, {"view", "growth"}, {"limit", 20}}}});
        } else if (name == "native-hotspot") {
            steps.push_back(Json{{"op", "/doctor"}, {"args", {{"kind", "native.cpu"}, {"deep", true}}}});
            steps.push_back(Json{{"op", "/start"}, {"args", {{"kind", "native.cpu"}, {"duration_seconds", 15}}}});
            steps.push_back(
                Json{{"op", "/query"}, {"args", {{"job_id", "$start.job.id"}, {"view", "threads"}, {"limit", 20}}}}
            );
            steps.push_back(
                Json{
                    {"op", "/query"},
                    {"args", {{"job_id", "$start.job.id"}, {"view", "calltree-roots"}, {"limit", 20}}}
                }
            );
            steps.push_back(
                Json{{"op", "/query"}, {"args", {{"job_id", "$start.job.id"}, {"view", "hotspots"}, {"limit", 20}}}}
            );
            steps.push_back(
                Json{
                    {"op", "/query"},
                    {"args",
                     {{"job_id", "$start.job.id"},
                      {"view", "calltree-children"},
                      {"filter", "$calltree.node_id"},
                      {"limit", 20}}}
                }
            );
        } else if (name == "compare-before-after") {
            steps.push_back(Json{{"op", "/history"}, {"args", {{"limit", 10}}}});
            steps.push_back(Json{{"op", "/query"}, {"args", {{"job_id", "$history.jobs[0].id"}, {"view", "hotspots"}, {"limit", 20}}}});
            steps.push_back(Json{{"op", "/query"}, {"args", {{"job_id", "$history.jobs[1].id"}, {"view", "hotspots"}, {"limit", 20}}}});
        } else {
            steps.push_back(Json{{"op", "/start"}, {"args", {{"kind", "python.cpu"}, {"duration_seconds", 15}}}});
            steps.push_back(Json{{"op", "/query"}, {"args", {{"job_id", "$start.job.id"}, {"view", "hotspots"}, {"limit", 20}}}});
        }

        Json data = {
            {"guide", name},
            {"steps", std::move(steps)},
            {"constraints",
             Json::array(
                 {"Do not start another profiler while a job is active.",
                  "Use returned job ids and cursors; do not invent them.",
                  "Inspect summary coverage and truncation before drawing conclusions."}
             )},
        };
        if (name == "native-hotspot") {
            data["goal"] =
                "Locate the exact instrumented Python or C++ stage responsible for a stall, including lower-level "
                "data-driven JSON work when corresponding engine zones are present.";
            data["result_references"] = "Replace $start.job.id and $calltree.node_id with ids returned by earlier "
                                        "calls; never send placeholders literally.";
            data["interpretation"]    = Json::array(
                {"Start from the affected thread and expand the dominant branch before using global hotspot rank.",
                    "A parent with high total but low self delegates its time to children; continue downward.",
                    "A node with high self is the current bottleneck candidate; inspect its source location and siblings.",
                    "For JSON-driven loading, distinguish parsing, conversion, construction, and dispatch by their child "
                       "zones.",
                    "Treat missing zones as missing evidence, not proof that a stage is free."}
            );
        }
        return makeToolResult(
            makeEnvelope(true, "/guide", std::move(data), nullptr, Json::array(), Json::array()),
            "mc_profiler guide is available in structuredContent."
        );
    }

} // namespace

namespace mcdk::mc_profiler_mcp {

    namespace {

        using namespace performance;

        Json jobJson(const JobSnapshot& job) {
            return Json{
                {"id", job.id},
                {"kind", toString(job.kind)},
                {"state", toString(job.state)},
                {"partial", job.partial},
                {"status_message", job.statusMessage},
                {"created_at", job.createdAt},
                {"completed_at", job.completedAt.empty() ? Json(nullptr) : Json(job.completedAt)},
            };
        }

        Json capabilitiesJson(const Capabilities& capabilities) {
            Json entries = Json::array();
            for (const auto& entry : capabilities.entries) {
                entries.push_back({
                    {"kind", toString(entry.kind)},
                    {"available", entry.available},
                    {"reason", entry.reason},
                });
            }
            return Json{{"capabilities", std::move(entries)}};
        }

        Json warningsJson(const Capabilities& capabilities) {
            Json warnings = Json::array();
            for (const auto& warning : capabilities.warnings) {
                warnings.push_back({{"code", "RUNTIME_NOTICE"}, {"message", warning}});
            }
            return warnings;
        }

        Json recordJson(const QueryRecord& record) {
            Json fields = Json::object();
            for (const auto& [name, field] : record.fields) {
                Json value = std::visit([](const auto& item) -> Json { return item; }, field.value);
                fields[name] = {{"value", std::move(value)}, {"unit", field.unit.empty() ? Json(nullptr) : Json(field.unit)}};
            }
            return Json{{"id", record.id}, {"fields", std::move(fields)}};
        }

        Json domainError(std::string_view op, const ProfilerError& error) {
            return buildErrorResult(op, error.code, error.message, error.retryable);
        }

        std::optional<ProfilerKind> parseKind(const Json& value) {
            if (!value.is_string()) return std::nullopt;
            const auto& kind = value.get_ref<const std::string&>();
            if (kind == "python.cpu") return ProfilerKind::PythonCpu;
            if (kind == "python.memory") return ProfilerKind::PythonMemory;
            if (kind == "native.cpu") return ProfilerKind::NativeCpu;
            return std::nullopt;
        }

        std::optional<ProfileTarget> parseTarget(const Json& value) {
            if (!value.is_string()) return std::nullopt;
            const auto& target = value.get_ref<const std::string&>();
            if (target == "client") return ProfileTarget::Client;
            if (target == "server") return ProfileTarget::Server;
            if (target == "all") return ProfileTarget::All;
            return std::nullopt;
        }

        Json successResult(std::string_view op, Json data, Json job, Json warnings, Json nextCalls, std::string summary) {
            auto envelope = makeEnvelope(true, op, std::move(data), nullptr, std::move(warnings), std::move(nextCalls));
            envelope["job"] = std::move(job);
            return makeToolResult(std::move(envelope), std::move(summary));
        }

        std::string pathUtf8(const std::filesystem::path& path) {
            const auto value = path.generic_u8string();
            return std::string(reinterpret_cast<const char*>(value.data()), value.size());
        }

    } // namespace

    std::optional<Json> tryBuildLocalResult(const Json& arguments) {
        if (!hasOnlyFields(arguments, {"op", "args"})) {
            return staticArgumentError("", "Arguments must be an object containing only 'op' and optional 'args'.");
        }
        const auto opIt = arguments.find("op");
        if (opIt == arguments.end() || !opIt->is_string() || opIt->get_ref<const std::string&>().empty()) {
            return staticArgumentError("", "Field 'op' is required and must be a non-empty string.");
        }
        const std::string op   = *opIt;
        if (op.size() > 64) {
            return staticArgumentError("", "Field 'op' must not exceed 64 bytes.");
        }
        const Json        args = arguments.value("args", Json::object());
        if (!args.is_object()) {
            return staticArgumentError(op, "Field 'args' must be an object when provided.");
        }
        if (!isSupportedOperation(op)) {
            return buildErrorResult(
                op,
                "UNKNOWN_OPERATION",
                "Unknown mc_profiler operation. Use /help to list supported operations.",
                false
            );
        }
        if (op == "/help") {
            if (!hasOnlyFields(args, {"topic"}) || (args.contains("topic") && (!args["topic"].is_string()
                || args["topic"].get_ref<const std::string&>().size() > 64))) {
                return staticArgumentError(op, "/help args may only contain an optional string field 'topic'.");
            }
            return buildHelp(args.value("topic", ""));
        }
        if (op == "/guide") {
            if (!hasOnlyFields(args, {"name"}) || (args.contains("name") && (!args["name"].is_string()
                || args["name"].get_ref<const std::string&>().size() > 64))) {
                return staticArgumentError(op, "/guide args may only contain an optional string field 'name'.");
            }
            return buildGuide(args.value("name", ""));
        }
        return std::nullopt;
    }

    Json buildErrorResult(std::string_view op, std::string_view code, std::string_view message, bool retryable) {
        Json envelope = makeEnvelope(
            false,
            op,
            nullptr,
            Json{
                {"code", code},
                {"message", message},
                {"retryable", retryable},
                {"details", Json::object()},
            },
            Json::array(),
            Json::array()
        );
        return makeToolResult(std::move(envelope), std::string(message));
    }

    bool validateProfilerEnvelope(const Json& envelope, std::string& error) {
        if (!hasOnlyFields(envelope, {"ok", "op", "job", "data", "error", "warnings", "next_calls"})) {
            error = "Envelope contains an unknown field or is not an object.";
            return false;
        }
        for (const auto required : {"ok", "op", "job", "data", "error", "warnings", "next_calls"}) {
            if (!envelope.contains(required)) {
                error = std::string("Envelope is missing '") + required + "'.";
                return false;
            }
        }
        if (!envelope["ok"].is_boolean() || !envelope["op"].is_string() || !envelope["warnings"].is_array()
            || !envelope["next_calls"].is_array()) {
            error = "Envelope field types are invalid.";
            return false;
        }
        if ((!envelope["job"].is_object() && !envelope["job"].is_null())
            || (!envelope["data"].is_object() && !envelope["data"].is_array() && !envelope["data"].is_null())) {
            error = "Envelope job or data field type is invalid.";
            return false;
        }
        for (const auto& warning : envelope["warnings"]) {
            if (!warning.is_object()) {
                error = "Envelope warnings must contain objects.";
                return false;
            }
        }
        if (envelope["next_calls"].size() > 3) {
            error = "Envelope contains more than three next_calls.";
            return false;
        }
        for (const auto& next : envelope["next_calls"]) {
            if (!hasOnlyFields(next, {"op", "args", "reason"}) || !next.contains("op") || !next["op"].is_string()
                || !next.contains("args") || !next["args"].is_object() || !next.contains("reason")
                || !next["reason"].is_string()) {
                error = "Envelope next_calls entries must contain only string op, object args, and string reason.";
                return false;
            }
        }
        if (envelope["ok"].get<bool>() && !envelope["error"].is_null()) {
            error = "Successful envelopes must have a null error.";
            return false;
        }
        if (!envelope["ok"].get<bool>() && !envelope["error"].is_object()) {
            error = "Failed envelopes must contain an error object.";
            return false;
        }
        if (!envelope["ok"].get<bool>()) {
            const auto& failure = envelope["error"];
            if (!hasOnlyFields(failure, {"code", "message", "retryable", "details"}) || !failure.contains("code")
                || !failure["code"].is_string() || !failure.contains("message") || !failure["message"].is_string()
                || !failure.contains("retryable") || !failure["retryable"].is_boolean() || !failure.contains("details")
                || !failure["details"].is_object()) {
                error = "Envelope error objects must contain code, message, retryable, and details.";
                return false;
            }
        }
        return true;
    }

    Json handleRuntimeRequest(ProfilerServiceProvider& provider, const Json& arguments) {
        auto service = provider.get();
        if (!service) return domainError(arguments.value("op", ""), service.error());

        if (auto local = tryBuildLocalResult(arguments)) {
            if ((*local)["structuredContent"].value("ok", false)) {
                auto capabilities = (*service)->inspectCapabilities({});
                if (capabilities) {
                    auto& envelope = (*local)["structuredContent"];
                    envelope["data"]["runtime"] = capabilitiesJson(*capabilities);
                    envelope["warnings"] = warningsJson(*capabilities);
                    (*local)["content"] = Json::array({Json{
                        {"type", "text"},
                        {"text", "mc_profiler help includes the runtime capability probe in structuredContent."},
                    }});
                }
            }
            return std::move(*local);
        }

        const auto& op = arguments.at("op").get_ref<const std::string&>();
        const Json args = arguments.value("args", Json::object());
        if (!args.is_object()) return staticArgumentError(op, "Field 'args' must be an object when provided.");

        if (op == "/doctor") {
            if (!hasOnlyFields(args, {"kind", "deep"})
                || (args.contains("deep") && !args["deep"].is_boolean())
                || (args.contains("kind") && !args["kind"].is_string())) {
                return staticArgumentError(op, "/doctor accepts only string kind and boolean deep.");
            }
            DoctorRequest request;
            if (args.contains("kind")) {
                request.kind = parseKind(args["kind"]);
                if (!request.kind) return staticArgumentError(op, "kind must be python.cpu, python.memory, or native.cpu.");
            }
            request.deep = args.value("deep", false);
            if (request.deep && (!request.kind || *request.kind != ProfilerKind::NativeCpu)) {
                return staticArgumentError(op, "deep=true is only valid with kind=native.cpu.");
            }
            auto result = (*service)->inspectCapabilities(request);
            if (!result) return domainError(op, result.error());
            Json next = Json::array();
            next.push_back(nextCall("/start", Json{{"kind", request.kind ? toString(*request.kind) : "python.cpu"}}, "Start a bounded capture after checking availability."));
            return successResult(op, capabilitiesJson(*result), nullptr, warningsJson(*result), std::move(next), "Profiler capabilities are available in structuredContent.");
        }

        if (op == "/start") {
            if (!hasOnlyFields(args, {"kind", "target", "clock", "duration_seconds", "traceback_depth", "collect_garbage"})
                || !args.contains("kind") || !args["kind"].is_string()) {
                return staticArgumentError(op, "/start requires kind and accepts only bounded profiler options.");
            }
            StartRequest request;
            const auto kind = parseKind(args["kind"]);
            if (!kind) return staticArgumentError(op, "kind must be python.cpu, python.memory, or native.cpu.");
            request.kind = *kind;
            if (args.contains("target")) {
                const auto target = parseTarget(args["target"]);
                if (!target) return staticArgumentError(op, "target must be client, server, or all.");
                request.target = *target;
            }
            if (args.contains("clock")) {
                if (!args["clock"].is_string()) return staticArgumentError(op, "clock must be cpu or wall.");
                const auto clock = args["clock"].get<std::string>();
                if (clock != "cpu" && clock != "wall") return staticArgumentError(op, "clock must be cpu or wall.");
                request.clock = clock == "cpu" ? ProfileClock::Cpu : ProfileClock::Wall;
            }
            if (args.contains("duration_seconds")) {
                if (!args["duration_seconds"].is_number_integer()) return staticArgumentError(op, "duration_seconds must be an integer.");
                const auto duration = args["duration_seconds"].get<std::int64_t>();
                if (duration < 1 || duration > 300) return staticArgumentError(op, "duration_seconds must be between 1 and 300.");
                request.duration = std::chrono::seconds(duration);
            }
            if (args.contains("traceback_depth")) {
                if (!args["traceback_depth"].is_number_unsigned() && !args["traceback_depth"].is_number_integer()) {
                    return staticArgumentError(op, "traceback_depth must be an integer from 1 to 16.");
                }
                const auto depth = args["traceback_depth"].get<std::int64_t>();
                if (depth < 1 || depth > 16) return staticArgumentError(op, "traceback_depth must be an integer from 1 to 16.");
                request.tracebackDepth = static_cast<std::size_t>(depth);
            }
            if (args.contains("collect_garbage")) {
                if (!args["collect_garbage"].is_boolean()) return staticArgumentError(op, "collect_garbage must be boolean.");
                request.collectGarbage = args["collect_garbage"].get<bool>();
            }
            auto result = (*service)->start(request);
            if (!result) return domainError(op, result.error());
            Json next = Json::array({nextCall("/status", Json{{"job_id", result->id}}, "Observe automatic finalization without extending the deadline.")});
            return successResult(op, Json{{"deadline_seconds", request.duration.count()}}, jobJson(*result), Json::array(), std::move(next), "Profiler job " + result->id + " started with a server deadline.");
        }

        if (op == "/status" || op == "/stop" || op == "/discard") {
            if (!hasOnlyFields(args, {"job_id"}) || !args.contains("job_id") || !args["job_id"].is_string()) {
                return staticArgumentError(op, op + " requires only string job_id.");
            }
            const auto jobId = args["job_id"].get<std::string>();
            if (jobId.empty() || jobId.size() > 128) return staticArgumentError(op, "job_id is invalid.");
            if (op == "/discard") {
                auto result = (*service)->discard(jobId);
                if (!result) return domainError(op, result.error());
                return successResult(op, Json{{"discard_requested", true}}, nullptr, Json::array(), Json::array(), "Profiler discard was accepted.");
            }
            auto result = op == "/stop" ? (*service)->stop(jobId) : (*service)->status(jobId);
            if (!result) return domainError(op, result.error());
            Json next = Json::array();
            if (result->state == JobState::Completed) next.push_back(nextCall("/query", Json{{"job_id", jobId}, {"view", "hotspots"}}, "Inspect the server-ranked bounded result."));
            else next.push_back(nextCall("/status", Json{{"job_id", jobId}}, "Check finalization without extending the deadline."));
            return successResult(op, Json::object(), jobJson(*result), Json::array(), std::move(next), "Profiler job status is available in structuredContent.");
        }

        if (op == "/query") {
            if (!hasOnlyFields(args, {"job_id", "view", "filter", "sort", "order", "limit", "cursor"})
                || !args.contains("job_id") || !args["job_id"].is_string()
                || !args.contains("view") || !args["view"].is_string()) {
                return staticArgumentError(op, "/query requires string job_id and view plus optional bounded query fields.");
            }
            QueryRequest request{.jobId = args["job_id"].get<std::string>(), .view = args["view"].get<std::string>()};
            if (request.jobId.empty() || request.jobId.size() > 128 || request.view.empty() || request.view.size() > 64) {
                return staticArgumentError(op, "job_id or view is invalid.");
            }
            if (args.contains("filter")) {
                if (!args["filter"].is_string() || args["filter"].get_ref<const std::string&>().size() > 256) return staticArgumentError(op, "filter must be a string of at most 256 bytes.");
                request.filter = args["filter"].get<std::string>();
            }
            if (args.contains("sort")) {
                if (!args["sort"].is_string() || args["sort"].get_ref<const std::string&>().size() > 64) return staticArgumentError(op, "sort must be a short field name.");
                request.sort = args["sort"].get<std::string>();
            }
            if (args.contains("order")) {
                if (!args["order"].is_string()) return staticArgumentError(op, "order must be asc or desc.");
                const auto order = args["order"].get<std::string>();
                if (order != "asc" && order != "desc") return staticArgumentError(op, "order must be asc or desc.");
                request.descending = order == "desc";
            }
            if (args.contains("limit")) {
                if (!args["limit"].is_number_integer()) return staticArgumentError(op, "limit must be an integer from 1 to 50.");
                const auto limit = args["limit"].get<std::int64_t>();
                if (limit < 1 || limit > 50) return staticArgumentError(op, "limit must be an integer from 1 to 50.");
                request.limit = static_cast<std::size_t>(limit);
            }
            if (args.contains("cursor")) {
                if (!args["cursor"].is_string() || args["cursor"].get_ref<const std::string&>().size() > 256) return staticArgumentError(op, "cursor must be a bounded string.");
                request.cursor = args["cursor"].get<std::string>();
            }
            auto result = (*service)->query(request);
            if (!result) return domainError(op, result.error());
            Json records = Json::array();
            for (const auto& record : result->records) records.push_back(recordJson(record));
            Json data{
                {"view", request.view}, {"records", std::move(records)}, {"total_available", result->totalAvailable},
                {"returned", result->records.size()}, {"truncated", result->truncated},
                {"next_cursor", result->nextCursor ? Json(*result->nextCursor) : Json(nullptr)},
            };
            Json next = Json::array();
            if (result->nextCursor) next.push_back(nextCall("/query", Json{{"job_id", request.jobId}, {"view", request.view}, {"cursor", *result->nextCursor}}, "Continue the same server-bound query."));
            return successResult(op, std::move(data), nullptr, Json::array(), std::move(next), "Bounded profiler records are available in structuredContent.");
        }

        if (op == "/detail") {
            if (!hasOnlyFields(args, {"job_id", "view", "record_id"}) || !args.contains("job_id") || !args["job_id"].is_string()
                || !args.contains("view") || !args["view"].is_string() || !args.contains("record_id") || !args["record_id"].is_string()) {
                return staticArgumentError(op, "/detail requires only string job_id, view, and record_id.");
            }
            DetailRequest request{args["job_id"].get<std::string>(), args["view"].get<std::string>(), args["record_id"].get<std::string>()};
            if (request.jobId.empty() || request.jobId.size() > 128 || request.view.empty() || request.view.size() > 64
                || request.recordId.empty() || request.recordId.size() > 256) {
                return staticArgumentError(op, "job_id, view, or record_id is invalid.");
            }
            auto result = (*service)->detail(request);
            if (!result) return domainError(op, result.error());
            Json related = Json::array();
            for (const auto& record : result->related) related.push_back(recordJson(record));
            return successResult(op, Json{{"record", recordJson(result->record)}, {"related", std::move(related)}, {"truncated", result->truncated}}, nullptr, Json::array(), Json::array(), "Profiler record detail is available in structuredContent.");
        }

        if (op == "/history") {
            if (!hasOnlyFields(args, {"limit", "cursor"})) return staticArgumentError(op, "/history accepts only limit and cursor.");
            HistoryRequest request;
            if (args.contains("limit")) {
                if (!args["limit"].is_number_integer()) return staticArgumentError(op, "limit must be an integer from 1 to 50.");
                const auto limit = args["limit"].get<std::int64_t>();
                if (limit < 1 || limit > 50) return staticArgumentError(op, "limit must be an integer from 1 to 50.");
                request.limit = static_cast<std::size_t>(limit);
            }
            if (args.contains("cursor")) {
                if (!args["cursor"].is_string() || args["cursor"].get_ref<const std::string&>().size() > 256) {
                    return staticArgumentError(op, "cursor must be a string of at most 256 bytes.");
                }
                request.cursor = args["cursor"].get<std::string>();
            }
            auto result = (*service)->history(request);
            if (!result) return domainError(op, result.error());
            Json jobs = Json::array();
            for (const auto& job : result->jobs) jobs.push_back(jobJson(job));
            return successResult(op, Json{{"jobs", std::move(jobs)}, {"next_cursor", result->nextCursor ? Json(*result->nextCursor) : Json(nullptr)}}, nullptr, Json::array(), Json::array(), "Profiler history is available in structuredContent.");
        }

        if (op == "/export") {
            if (!hasOnlyFields(args, {"job_id", "format"}) || !args.contains("job_id") || !args["job_id"].is_string()
                || !args.contains("format") || !args["format"].is_string()) {
                return staticArgumentError(op, "/export requires only string job_id and format.");
            }
            const auto format = args["format"].get<std::string>();
            if (format != "markdown" && format != "svg") return staticArgumentError(op, "format must be markdown or svg.");
            if (args["job_id"].get_ref<const std::string&>().empty()
                || args["job_id"].get_ref<const std::string&>().size() > 128) {
                return staticArgumentError(op, "job_id is invalid.");
            }
            ExportRequest request{args["job_id"].get<std::string>(), format == "svg" ? ExportFormat::Svg : ExportFormat::Markdown};
            auto result = (*service)->exportReport(request);
            if (!result) return domainError(op, result.error());
            return successResult(op, Json{{"artifact", {{"path", pathUtf8(result->path)}, {"size", result->size}, {"sha256", result->sha256}}}}, nullptr, Json::array(), Json::array(), "Profiler report was written to a server-controlled path.");
        }

        if (op == "/cleanup") {
            if (!hasOnlyFields(args, {"dry_run"}) || (args.contains("dry_run") && !args["dry_run"].is_boolean())) return staticArgumentError(op, "/cleanup accepts only boolean dry_run.");
            auto result = (*service)->cleanup({args.value("dry_run", false)});
            if (!result) return domainError(op, result.error());
            return successResult(op, Json{{"dry_run", args.value("dry_run", false)}, {"removed_jobs", result->removedJobs}, {"removed_bytes", result->removedBytes}}, nullptr, Json::array(), Json::array(), "Profiler retention cleanup completed.");
        }

        return buildErrorResult(op, "UNKNOWN_OPERATION", "Unknown mc_profiler operation. Use /help.", false);
    }

} // namespace mcdk::mc_profiler_mcp

namespace mcdk::mcp_tool_definitions {

    mcp::tool buildMcProfilerTool() {
        mcp::tool tool;
        tool.name = std::string(mc_profiler_mcp::ToolName);
        tool.description =
            "Profiles Minecraft Python CPU, Python memory, and optional Native CPU through one bounded command tool. "
            "Native profiles can correlate instrumented Python-facing and engine C++ Tracy zone hierarchies, including "
            "lower-level stages such as data-driven JSON parsing when those zones are emitted. Call /help first. Every "
            "capture has a server deadline; results are filtered and paged. Input uses {op:'/...', args:{...}}.";
        tool.parameters_schema = {
            {"type", "object"},
            {"required", Json::array({"op"})},
            {"properties",
             {{"op", {{"type", "string"}, {"description", "Operation such as /help, /doctor, /start, or /query."}}},
              {"args", {{"type", "object"}, {"description", "Strict operation-specific arguments."}}}}},
            {"additionalProperties", false},
        };
        tool.output_schema = {
            {"type", "object"},
            {"required", Json::array({"ok", "op", "job", "data", "error", "warnings", "next_calls"})},
            {"properties",
             {{"ok", {{"type", "boolean"}}},
              {"op", {{"type", "string"}}},
              {"job", {{"type", Json::array({"object", "null"})}}},
              {"data", {{"type", Json::array({"object", "array", "null"})}}},
              {"error", {{"type", Json::array({"object", "null"})}}},
              {"warnings", {{"type", "array"}, {"items", {{"type", "object"}}}}},
              {"next_calls", {{"type", "array"}, {"maxItems", 3}, {"items", {{"type", "object"}}}}}}},
            {"additionalProperties", false},
        };
        tool.annotations.read_only_hint   = false;
        tool.annotations.destructive_hint = true;
        tool.annotations.idempotent_hint  = false;
        tool.annotations.open_world_hint  = true;
        return tool;
    }

} // namespace mcdk::mcp_tool_definitions
