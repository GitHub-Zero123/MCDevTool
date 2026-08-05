#include <performance/profiler_service_factory.hpp>

#include <performance/native_bridge_loader.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <thread>
#include <type_traits>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace mcdk::performance {
namespace {

    using Json  = nlohmann::json;
    using Clock = std::chrono::steady_clock;

    constexpr std::size_t MaximumQueryRecords = 50;
    constexpr std::size_t MaximumQueryBytes   = 64 * 1024;

    ProfilerError failure(std::string code, std::string message, bool retryable = false) {
        if (code.size() > 128) code.resize(128);
        if (message.size() > 4096) message.resize(4096);
        return {.code = std::move(code), .message = std::move(message), .retryable = retryable};
    }

    std::string utcNow() {
        const auto now = std::chrono::system_clock::now();
        const auto value = std::chrono::system_clock::to_time_t(now);
        std::tm parts{};
#ifdef _WIN32
        gmtime_s(&parts, &value);
#else
        gmtime_r(&value, &parts);
#endif
        std::ostringstream output;
        output << std::put_time(&parts, "%Y-%m-%dT%H:%M:%SZ");
        return output.str();
    }

    std::string makeJobId() {
        static std::atomic<std::uint64_t> sequence = 0;
        const auto ticks = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
        std::random_device random;
        std::ostringstream output;
        output << std::hex << ticks << '-' << sequence.fetch_add(1, std::memory_order_relaxed) << '-'
               << static_cast<std::uint32_t>(random());
        return output.str();
    }

    std::string replaceToken(std::string value, std::string_view token, std::string_view replacement) {
        std::size_t offset = 0;
        while ((offset = value.find(token, offset)) != std::string::npos) {
            value.replace(offset, token.size(), replacement);
            offset += replacement.size();
        }
        return value;
    }

    std::string pythonCpuStartCode(const StartRequest& request, std::string_view owner) {
        std::string code = R"PY(import yappi,threading,time
_mcdev_pp_clock='@CLOCK@'
_mcdev_pp_duration=@DURATION@
_mcdev_pp_owner='@OWNER@'
if yappi.is_running() or globals().get('_mcdev_pp_owned',False):
 _result={'ok':False,'reason':'busy'}
else:
 _old=globals().get('_mcdev_pp_timer')
 if _old: _old.cancel()
 _ttl=globals().get('_mcdev_pp_ttl')
 if _ttl: _ttl.cancel()
 yappi.clear_stats()
 yappi.set_clock_type(_mcdev_pp_clock)
 yappi.start(False,@THREADS@)
 globals()['_mcdev_pp_owned']=True; globals()['_mcdev_pp_owner']=_mcdev_pp_owner
 globals()['_mcdev_pp_started']=time.time()
 globals()['_mcdev_pp_stopped']=None
 globals()['_mcdev_pp_clock']=_mcdev_pp_clock
 def _mcdev_pp_stop(_owner=_mcdev_pp_owner):
  try:
   if globals().get('_mcdev_pp_owner')==_owner and globals().get('_mcdev_pp_owned',False) and yappi.is_running():
    yappi.stop(); globals()['_mcdev_pp_stopped']=time.time()
  except: pass
 def _mcdev_pp_expire(_owner=_mcdev_pp_owner):
  try:
   if globals().get('_mcdev_pp_owner')==_owner and globals().get('_mcdev_pp_owned',False):
    if yappi.is_running(): yappi.stop()
    yappi.clear_stats(); globals()['_mcdev_pp_owned']=False; globals()['_mcdev_pp_owner']=None
  except: pass
 _timer=threading.Timer(_mcdev_pp_duration,_mcdev_pp_stop); _timer.daemon=True; _timer.start()
 _ttl=threading.Timer(_mcdev_pp_duration+60,_mcdev_pp_expire); _ttl.daemon=True; _ttl.start()
 globals()['_mcdev_pp_timer']=_timer; globals()['_mcdev_pp_ttl']=_ttl
 _result={'ok':True,'running':True,'clock':_mcdev_pp_clock})PY";
        code = replaceToken(std::move(code), "@CLOCK@", request.clock == ProfileClock::Wall ? "WALL" : "CPU");
        code = replaceToken(std::move(code), "@DURATION@", std::to_string(request.duration.count()));
        code = replaceToken(std::move(code), "@OWNER@", owner);
        return replaceToken(std::move(code), "@THREADS@", request.target == ProfileTarget::All ? "True" : "False");
    }

    std::string pythonCpuMarkerCode(ProfileTarget side) {
        const auto name = side == ProfileTarget::Client ? "client" : "server";
        return "def _mcdev_pp_" + std::string(name) + "_marker(): pass\n_mcdev_pp_" + name
             + "_marker()\n_result=True";
    }

    std::string pythonCpuCollectCode(const StartRequest& request, std::string_view owner) {
        const auto target = request.target == ProfileTarget::Client ? "client"
                          : request.target == ProfileTarget::Server ? "server" : "all";
        std::string code = R"PY(import yappi,time
_mcdev_pp_owner='@OWNER@'
if globals().get('_mcdev_pp_owner')!=_mcdev_pp_owner or not globals().get('_mcdev_pp_owned',False):
 _result={'ok':False,'reason':'not_owned'}
else:
 _timer=globals().get('_mcdev_pp_timer'); _ttl=globals().get('_mcdev_pp_ttl')
 if _timer: _timer.cancel()
 if _ttl: _ttl.cancel()
 if yappi.is_running(): yappi.stop(); globals()['_mcdev_pp_stopped']=time.time()
 _stats=yappi.get_func_stats(); _stats.sort('ttot','desc')
 _ctx={}
 for _s in _stats:
  if _s.name=='_mcdev_pp_client_marker': _ctx[int(_s.ctx_id or 0)]='client'
  elif _s.name=='_mcdev_pp_server_marker': _ctx[int(_s.ctx_id or 0)]='server'
 try:
  import common.minecraftMod as _mod
  _inst=_mod.instance()
  _scripts=set(_n for _n in ((getattr(_inst,'clientScriptNameList',[]) or [])+(getattr(_inst,'serverScriptNameList',[]) or [])) if _n)
 except: _scripts=[]
 _all=[]; _sides={}
 for _s in _stats:
  _module=_s.module or ''; _parts=set(_module.replace('\\','/').split('/'))
  _project=any(_n in _parts or _module==_n or _module.startswith(_n+'.') for _n in _scripts)
  _side=_ctx.get(int(_s.ctx_id or 0)) if '@TARGET@'=='all' else '@TARGET@'
  if '@TARGET@'=='all': _project=_project and _side is not None
  if _project and not _s.name.startswith('_mcdev_pp_'): _all.append(_s); _sides[_s.index]=_side
 _keep=_all[:512]; _ids=dict((_s.index,_i) for _i,_s in enumerate(_keep)); _nodes=[]
 for _i,_s in enumerate(_keep):
  _nodes.append([_i,(_s.module or '')[:4096],int(_s.lineno or 0),(_s.name or '')[:1024],int(_s.ncall or 0),int(_s.nactualcall or 0),float(_s.tsub or 0),float(_s.ttot or 0),int(_s.ctx_id or 0),(_s.ctx_name or '')[:512],_sides.get(_s.index)])
 _edges=[]
 for _parent in _keep:
  for _child in _parent.children:
   if _child.index in _ids and _sides.get(_parent.index)==_sides.get(_child.index):
    _edges.append([_ids[_parent.index],_ids[_child.index],int(_child.ncall or 0),float(_child.tsub or 0),float(_child.ttot or 0)])
    if len(_edges)>=2048: break
  if len(_edges)>=2048: break
 _end=globals().get('_mcdev_pp_stopped') or time.time()
 _result={'ok':True,'clock':globals().get('_mcdev_pp_clock','CPU'),'elapsed':max(0,_end-globals().get('_mcdev_pp_started',_end)),'total':len(_all),'truncated':len(_all)>len(_keep) or len(_edges)>=2048,'targets':list(set(_ctx.values())),'nodes':_nodes,'edges':_edges}
 yappi.clear_stats(); globals()['_mcdev_pp_owned']=False; globals()['_mcdev_pp_owner']=None; globals()['_mcdev_pp_timer']=None; globals()['_mcdev_pp_ttl']=None)PY";
        code = replaceToken(std::move(code), "@OWNER@", owner);
        return replaceToken(std::move(code), "@TARGET@", target);
    }

    std::string pythonCpuCleanupCode(std::string_view owner) {
        std::string code = R"PY(import yappi
_mcdev_pp_owner='@OWNER@'
_timer=globals().get('_mcdev_pp_timer'); _ttl=globals().get('_mcdev_pp_ttl')
if globals().get('_mcdev_pp_owner')==_mcdev_pp_owner:
 if _timer: _timer.cancel()
 if _ttl: _ttl.cancel()
 if yappi.is_running(): yappi.stop()
 yappi.clear_stats()
 globals()['_mcdev_pp_owned']=False; globals()['_mcdev_pp_owner']=None; globals()['_mcdev_pp_timer']=None; globals()['_mcdev_pp_ttl']=None
_result=True)PY";
        return replaceToken(std::move(code), "@OWNER@", owner);
    }

    std::string pythonMemoryStartCode(const StartRequest& request, std::string_view owner) {
        std::string code = R"PY(import tracemalloc,time,threading
_mcdev_pm_owner='@OWNER@'
if tracemalloc.is_tracing() or globals().get('_mcdev_pm_owned',False):
 _result={'ok':False,'reason':'busy'}
else:
 _ttl=globals().get('_mcdev_pm_ttl')
 if _ttl: _ttl.cancel()
 tracemalloc.start(@DEPTH@)
 globals()['_mcdev_pm_owned']=True; globals()['_mcdev_pm_owner']=_mcdev_pm_owner; globals()['_mcdev_pm_depth']=@DEPTH@
 globals()['_mcdev_pm_started']=time.time(); globals()['_mcdev_pm_base']=tracemalloc.take_snapshot()
 def _mcdev_pm_expire(_owner=_mcdev_pm_owner):
  try:
   if globals().get('_mcdev_pm_owner')==_owner:
    if globals().get('_mcdev_pm_owned',False) and tracemalloc.is_tracing(): tracemalloc.stop()
    globals()['_mcdev_pm_owned']=False; globals()['_mcdev_pm_owner']=None; globals()['_mcdev_pm_base']=None
  except: pass
 _ttl=threading.Timer(@TTL@,_mcdev_pm_expire); _ttl.daemon=True; _ttl.start(); globals()['_mcdev_pm_ttl']=_ttl
 _result={'ok':True,'depth':@DEPTH@})PY";
        code = replaceToken(std::move(code), "@DEPTH@", std::to_string(request.tracebackDepth));
        code = replaceToken(std::move(code), "@TTL@", std::to_string(request.duration.count() + 60));
        return replaceToken(std::move(code), "@OWNER@", owner);
    }

    std::string pythonMemoryCollectCode(bool collectGarbage, std::string_view owner) {
        std::string code = R"PY(import tracemalloc,time,gc
_mcdev_pm_owner='@OWNER@'
if globals().get('_mcdev_pm_owner')!=_mcdev_pm_owner or not globals().get('_mcdev_pm_owned',False) or not tracemalloc.is_tracing():
 _result={'ok':False,'reason':'not_owned'}
else:
 _ttl=globals().get('_mcdev_pm_ttl')
 if _ttl: _ttl.cancel()
 @GC@
 _base=globals().get('_mcdev_pm_base'); _now=tracemalloc.take_snapshot(); _stats=_now.compare_to(_base,'traceback')
 try:
  import common.minecraftMod as _mod
  _inst=_mod.instance(); _scripts=set(_n for _n in ((getattr(_inst,'clientScriptNameList',[]) or [])+(getattr(_inst,'serverScriptNameList',[]) or [])) if _n)
 except: _scripts=[]
 _all=[]
 for _s in _stats:
  _project=False; _origin=(_s.traceback[0].filename or '').replace('\\','/').lower()
  if 'qumodlibs' in set(_origin.split('/')): continue
  for _f in _s.traceback:
   _file=_f.filename or ''; _norm=_file.replace('\\','/').lower(); _parts=set(_norm.split('/'))
   if any(_n.lower() in _parts or _norm==_n.lower() or _norm.startswith(_n.lower()+'.') for _n in _scripts): _project=True; break
  if _project: _all.append(_s)
 _all.sort(key=lambda _s:abs(_s.size_diff),reverse=True); _keep=_all[:512]; _rows=[]
 for _i,_s in enumerate(_keep):
  _frames=[[(_f.filename or '')[:1024],int(_f.lineno or 0)] for _f in _s.traceback]
  _rows.append([_i,int(_s.size_diff),int(_s.count_diff),int(_s.size),int(_s.count),_frames])
 _result={'ok':True,'elapsed':max(0,time.time()-globals().get('_mcdev_pm_started',time.time())),'depth':int(globals().get('_mcdev_pm_depth',1)),'sizeDiff':sum(_s.size_diff for _s in _all),'countDiff':sum(_s.count_diff for _s in _all),'size':sum(_s.size for _s in _all),'count':sum(_s.count for _s in _all),'total':len(_all),'truncated':len(_all)>len(_keep),'rows':_rows}
 tracemalloc.stop(); globals()['_mcdev_pm_owned']=False; globals()['_mcdev_pm_owner']=None; globals()['_mcdev_pm_base']=None; globals()['_mcdev_pm_ttl']=None)PY";
        code = replaceToken(std::move(code), "@GC@", collectGarbage ? "gc.collect()" : "pass");
        return replaceToken(std::move(code), "@OWNER@", owner);
    }

    std::string pythonMemoryCleanupCode(std::string_view owner) {
        std::string code = R"PY(import tracemalloc
_mcdev_pm_owner='@OWNER@'
_ttl=globals().get('_mcdev_pm_ttl')
if globals().get('_mcdev_pm_owner')==_mcdev_pm_owner:
 if _ttl: _ttl.cancel()
 if globals().get('_mcdev_pm_owned',False) and tracemalloc.is_tracing(): tracemalloc.stop()
 globals()['_mcdev_pm_owned']=False; globals()['_mcdev_pm_owner']=None; globals()['_mcdev_pm_base']=None; globals()['_mcdev_pm_ttl']=None
_result=True)PY";
        return replaceToken(std::move(code), "@OWNER@", owner);
    }

    bool validProfilerPayload(const Json& value) {
        return value.is_object() && value.value("ok", false);
    }

    std::string lower(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    std::string fieldText(const ProfilerField& field) {
        return std::visit([](const auto& item) {
            std::ostringstream output;
            output << std::boolalpha << item;
            return output.str();
        }, field.value);
    }

    std::string xmlEscape(std::string_view value) {
        std::string result;
        result.reserve(value.size());
        for (const char character : value) {
            switch (character) {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '\"': result += "&quot;"; break;
            case '\'': result += "&apos;"; break;
            default: result.push_back(character); break;
            }
        }
        return result;
    }

    std::optional<long double> numericFieldValue(const ProfilerField& field) {
        return std::visit([](const auto& value) -> std::optional<long double> {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, std::int64_t>) {
                return static_cast<long double>(value);
            } else if constexpr (std::is_same_v<Value, double>) {
                return std::isfinite(value) ? std::optional<long double>(value) : std::nullopt;
            } else {
                return std::nullopt;
            }
        }, field.value);
    }

    void addField(QueryRecord& record, std::string name, ProfilerFieldValue value, std::string unit = {}) {
        record.fields.emplace(std::move(name), ProfilerField{std::move(value), std::move(unit)});
    }

    std::size_t recordBytes(const QueryRecord& record) {
        std::size_t result = record.id.size() + 32;
        for (const auto& [name, field] : record.fields) result += name.size() + field.unit.size() + fieldText(field).size() + 24;
        return result;
    }

    std::expected<void, ProfilerError> writeAtomic(const std::filesystem::path& path, std::string_view contents) {
        static std::atomic<std::uint64_t> temporarySequence = 0;
        const auto temporary = path.string() + ".tmp-"
                             + std::to_string(temporarySequence.fetch_add(1, std::memory_order_relaxed));
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return std::unexpected(failure("PERSIST_OPEN_FAILED", "Unable to open a temporary profile artifact."));
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.flush();
        if (!output) {
            output.close();
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return std::unexpected(failure("PERSIST_WRITE_FAILED", "Unable to write a profile artifact."));
        }
        output.close();
        std::error_code error;
        if (std::filesystem::exists(path, error)) {
            std::filesystem::remove(temporary, error);
            return std::unexpected(failure("PERSIST_CONFLICT", "A profile artifact already exists."));
        }
        std::filesystem::rename(temporary, path, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            return std::unexpected(failure("PERSIST_COMMIT_FAILED", "Unable to commit a profile artifact atomically."));
        }
        return {};
    }

    std::string jsonString(const Json& value, std::string_view key, std::size_t maximum = 4096) {
        if (!value.is_object() || !value.contains(key) || !value[key].is_string()) return {};
        auto result = value[key].get<std::string>();
        if (result.size() > maximum) result.resize(maximum);
        return result;
    }

    std::int64_t jsonInteger(const Json& value, std::string_view key) {
        if (!value.is_object() || !value.contains(key) || !value[key].is_number()) return 0;
        return value[key].get<double>() < 0 ? 0 : static_cast<std::int64_t>(value[key].get<double>());
    }

    double jsonNumber(const Json& value, std::string_view key) {
        if (!value.is_object() || !value.contains(key) || !value[key].is_number()) return 0;
        return std::max(0.0, value[key].get<double>());
    }

} // namespace

class DefaultProfilerService final : public ProfilerService {
    struct Job {
        JobSnapshot snapshot;
        StartRequest request;
        Clock::time_point deadline;
        Clock::time_point lastAccess;
        std::condition_variable condition;
        std::atomic<bool> stopRequested = false;
        std::atomic<bool> discardRequested = false;
        Json data;
        Json summary;
        std::filesystem::path directory;
        std::filesystem::path temporaryTrace;
        NativeCaptureHandle nativeCapture;
        std::thread worker;
    };

public:
    explicit DefaultProfilerService(ProfilerServiceOptions options)
    : options_(std::move(options)), native_(options_.executableDirectory) {
        std::error_code error;
        options_.storageRoot = std::filesystem::absolute(options_.storageRoot, error).lexically_normal();
        if (error || options_.storageRoot.empty()) throw std::runtime_error("The profiler storage root is invalid.");
        std::filesystem::create_directories(options_.storageRoot, error);
        if (error) throw std::runtime_error("The profiler storage root cannot be created.");
        (void)native_.initialize();
    }

    ~DefaultProfilerService() override { shutdown(); }

    std::expected<JobSnapshot, ProfilerError> start(const StartRequest& request) override {
        collectExpiredMemoryJobs();
        if (request.duration < std::chrono::seconds(1) || request.duration > std::chrono::seconds(300)) {
            return std::unexpected(failure("INVALID_DURATION", "duration_seconds must be between 1 and 300."));
        }
        if (request.kind == ProfilerKind::PythonMemory && request.target != ProfileTarget::Client) {
            return std::unexpected(failure("INVALID_TARGET", "Python memory profiling only supports target=client."));
        }
        if (request.tracebackDepth < 1 || request.tracebackDepth > 16) {
            return std::unexpected(failure("INVALID_TRACEBACK_DEPTH", "traceback_depth must be between 1 and 16."));
        }

        auto job = std::make_shared<Job>();
        job->snapshot.id        = makeJobId();
        job->snapshot.kind      = request.kind;
        job->snapshot.storage   = request.storage;
        job->snapshot.state     = JobState::Starting;
        job->snapshot.createdAt = utcNow();
        job->request             = request;
        job->deadline            = Clock::now() + request.duration;
        job->lastAccess          = monotonicNow();
        if (request.storage == ProfileStorage::Disk) {
            job->directory = options_.storageRoot / job->snapshot.id;
        }
        job->temporaryTrace      = options_.storageRoot / ".runtime" / job->snapshot.id / "capture.tracy";

        {
            std::lock_guard lock(mutex_);
            if (shuttingDown_.load(std::memory_order_acquire)) {
                return std::unexpected(failure("PROFILER_STOPPED", "The profiler service is shutting down."));
            }
            if (active_) return std::unexpected(failure("PROFILER_BUSY", "Another profiler job is active.", true));
            jobs_.emplace(job->snapshot.id, job);
            active_ = job;
        }

        auto started = startBackend(*job);
        if (!started) {
            finishFailed(job, started.error());
            return std::unexpected(started.error());
        }
        {
            std::lock_guard lock(mutex_);
            job->snapshot.state = JobState::Running;
            job->snapshot.statusMessage = "Capture is running and will stop at the server deadline.";
        }
        job->worker = std::thread([this, job] { runJob(job); });
        return snapshotOf(job);
    }

    std::expected<JobSnapshot, ProfilerError> status(const JobId& id) const override {
        const auto job = findJob(id);
        if (!job) return std::unexpected(failure("JOB_NOT_FOUND", "Profiler job was not found."));
        return snapshotOf(job);
    }

    std::expected<JobSnapshot, ProfilerError> stop(const JobId& id) override {
        const auto job = findJob(id);
        if (!job) return std::unexpected(failure("JOB_NOT_FOUND", "Profiler job was not found."));
        {
            std::lock_guard lock(mutex_);
            if (job->snapshot.state == JobState::Running || job->snapshot.state == JobState::Starting) {
                job->stopRequested = true;
                job->snapshot.statusMessage = "Early stop requested; finalization keeps the capture result.";
                if (job->nativeCapture.value) native_.stop(job->nativeCapture);
            }
        }
        job->condition.notify_all();
        return snapshotOf(job);
    }

    std::expected<void, ProfilerError> discard(const JobId& id) override {
        const auto job = findJob(id);
        if (!job) return std::unexpected(failure("JOB_NOT_FOUND", "Profiler job was not found."));
        {
            std::lock_guard lock(mutex_);
            if (job->snapshot.state == JobState::Completed || job->snapshot.state == JobState::Failed
                || job->snapshot.state == JobState::Discarded || job->snapshot.state == JobState::Aborted) {
                std::error_code ignored;
                if (!job->directory.empty()) std::filesystem::remove_all(job->directory, ignored);
                job->snapshot.state = JobState::Discarded;
                job->snapshot.statusMessage = "Capture artifacts were discarded.";
                return {};
            }
            job->discardRequested = true;
            job->stopRequested = true;
            job->snapshot.statusMessage = "Discard requested; backend cleanup is in progress.";
            if (job->nativeCapture.value) native_.stop(job->nativeCapture);
        }
        job->condition.notify_all();
        return {};
    }

    std::expected<QueryPage, ProfilerError> query(const QueryRequest& request) const override {
        const auto job = findJob(request.jobId);
        if (!job) return std::unexpected(failure("JOB_NOT_FOUND", "Profiler job was not found."));
        const auto snapshot = snapshotOf(job);
        if (snapshot.state != JobState::Completed) {
            return std::unexpected(failure("JOB_NOT_QUERYABLE", "Profiler results are queryable only after completion.", true));
        }
        auto records = recordsFor(*job, request.view);
        if (!records) return std::unexpected(records.error());
        const auto filter = request.filter ? lower(*request.filter) : std::string{};
        if (request.view == "calltree-children") {
            if (filter.empty()) {
                return std::unexpected(failure(
                    "CALLTREE_PARENT_REQUIRED",
                    "calltree-children requires filter to be one complete parent node id."
                ));
            }
            std::erase_if(*records, [&](const QueryRecord& record) {
                const auto parent = record.fields.find("parent_id");
                return parent == record.fields.end() || lower(fieldText(parent->second)) != filter;
            });
        } else if (!filter.empty()) {
            std::erase_if(*records, [&](const QueryRecord& record) {
                if (lower(record.id).find(filter) != std::string::npos) return false;
                for (const auto& [name, field] : record.fields) {
                    if (lower(name + " " + fieldText(field)).find(filter) != std::string::npos) return false;
                }
                return true;
            });
        }
        const auto sortField = request.sort.empty() ? defaultSort(request.view) : request.sort;
        std::stable_sort(records->begin(), records->end(), [&](const QueryRecord& left, const QueryRecord& right) {
            const auto l = left.fields.find(sortField);
            const auto r = right.fields.find(sortField);
            if (l == left.fields.end() || r == right.fields.end()) {
                if (l == left.fields.end() && r == right.fields.end()) return false;
                return request.descending ? l != left.fields.end() : l == left.fields.end();
            }
            const auto ln = numericFieldValue(l->second);
            const auto rn = numericFieldValue(r->second);
            if (ln && rn) return request.descending ? *ln > *rn : *ln < *rn;
            const auto lv = fieldText(l->second);
            const auto rv = fieldText(r->second);
            return request.descending ? lv > rv : lv < rv;
        });

        const auto binding = request.jobId + "|" + request.view + "|" + filter + "|" + sortField
                           + (request.descending ? "|desc" : "|asc");
        std::size_t offset = 0;
        if (request.cursor) {
            const auto separator = request.cursor->find(':');
            if (separator == std::string::npos || request.cursor->substr(0, separator) != cursorSignature(binding)) {
                return std::unexpected(failure("CURSOR_INVALID", "Cursor does not belong to this query."));
            }
            const auto value = request.cursor->substr(separator + 1);
            const auto [pointer, error] = std::from_chars(value.data(), value.data() + value.size(), offset);
            if (error != std::errc{} || pointer != value.data() + value.size()) {
                return std::unexpected(failure("CURSOR_INVALID", "Cursor offset is invalid."));
            }
        }
        if (offset > records->size()) {
            return std::unexpected(failure("CURSOR_INVALID", "Cursor offset exceeds the available result set."));
        }
        QueryPage page;
        page.totalAvailable = records->size();
        const auto limit = std::clamp<std::size_t>(request.limit, 1, MaximumQueryRecords);
        std::size_t bytes = 0;
        for (std::size_t index = offset; index < records->size() && page.records.size() < limit; ++index) {
            const auto estimate = recordBytes((*records)[index]);
            if (page.records.empty() && estimate > MaximumQueryBytes) {
                return std::unexpected(failure("QUERY_RECORD_TOO_LARGE", "A profiler record exceeds the query response budget."));
            }
            if (!page.records.empty() && bytes + estimate > MaximumQueryBytes) break;
            bytes += estimate;
            page.records.push_back(std::move((*records)[index]));
        }
        const auto consumed = offset + page.records.size();
        page.truncated = consumed < records->size();
        if (page.truncated) page.nextCursor = cursorSignature(binding) + ':' + std::to_string(consumed);
        return page;
    }

    std::expected<DetailResult, ProfilerError> detail(const DetailRequest& request) const override {
        const auto job = findJob(request.jobId);
        if (!job) return std::unexpected(failure("JOB_NOT_FOUND", "Profiler job was not found."));
        if (snapshotOf(job).state != JobState::Completed) {
            return std::unexpected(failure("JOB_NOT_QUERYABLE", "Profiler details are available only after completion.", true));
        }
        auto records = recordsFor(*job, request.view);
        if (!records) return std::unexpected(records.error());
        const auto found = std::find_if(records->begin(), records->end(), [&](const auto& record) {
            return record.id == request.recordId;
        });
        if (found == records->end()) return std::unexpected(failure("RECORD_NOT_FOUND", "Profile record was not found."));
        DetailResult result{.record = *found};
        std::size_t bytes = recordBytes(result.record);
        if (bytes > MaximumQueryBytes) {
            return std::unexpected(failure("DETAIL_RECORD_TOO_LARGE", "The requested record exceeds the detail response budget."));
        }
        const auto parent = found->fields.find("parent_id");
        for (const auto& record : *records) {
            const auto candidate = record.fields.find("parent_id");
            const bool related = candidate != record.fields.end()
                && (fieldText(candidate->second) == request.recordId
                    || (parent != found->fields.end()
                        && fieldText(candidate->second) == fieldText(parent->second)
                        && record.id != request.recordId));
            if (!related) continue;
            if (result.related.size() == 20) {
                result.truncated = true;
                break;
            }
            const auto estimate = recordBytes(record);
            if (bytes + estimate > MaximumQueryBytes) {
                result.truncated = true;
                break;
            }
            bytes += estimate;
            result.related.push_back(record);
        }
        return result;
    }

    std::expected<HistoryPage, ProfilerError> history(const HistoryRequest& request) const override {
        collectExpiredMemoryJobs();
        scanHistoryOnce();
        std::vector<JobSnapshot> values;
        {
            std::lock_guard lock(mutex_);
            values = historyCache_;
            for (const auto& [id, job] : jobs_) {
                if (job->request.storage != ProfileStorage::Disk) continue;
                const auto snapshot = job->snapshot;
                const auto existing = std::find_if(values.begin(), values.end(), [&](const auto& item) { return item.id == id; });
                if (existing == values.end()) values.push_back(snapshot);
                else *existing = snapshot;
            }
        }
        std::sort(values.begin(), values.end(), [](const auto& left, const auto& right) { return left.createdAt > right.createdAt; });
        std::size_t offset = 0;
        if (request.cursor) {
            const auto [pointer, error] = std::from_chars(
                request.cursor->data(), request.cursor->data() + request.cursor->size(), offset
            );
            if (error != std::errc{} || pointer != request.cursor->data() + request.cursor->size()) {
                return std::unexpected(failure("CURSOR_INVALID", "History cursor is invalid."));
            }
        }
        if (offset > values.size()) {
            return std::unexpected(failure("CURSOR_INVALID", "History cursor exceeds the available result set."));
        }
        HistoryPage page;
        const auto limit = std::clamp<std::size_t>(request.limit, 1, MaximumQueryRecords);
        for (std::size_t index = offset; index < values.size() && page.jobs.size() < limit; ++index) page.jobs.push_back(values[index]);
        if (offset + page.jobs.size() < values.size()) page.nextCursor = std::to_string(offset + page.jobs.size());
        return page;
    }

    std::expected<ExportResult, ProfilerError> exportReport(const ExportRequest& request) override {
        const auto job = findJob(request.jobId);
        if (!job) return std::unexpected(failure("JOB_NOT_FOUND", "Profiler job was not found."));
        if (snapshotOf(job).state != JobState::Completed) {
            return std::unexpected(failure("JOB_NOT_EXPORTABLE", "Only completed jobs can be exported.", true));
        }
        const bool svg = request.format == ExportFormat::Svg;
        const auto reportDirectory = job->request.storage == ProfileStorage::Disk
                                   ? job->directory
                                   : options_.storageRoot / ".exports" / job->snapshot.id;
        const auto path = reportDirectory / (svg ? "report.svg" : "report.md");
        std::error_code createError;
        std::filesystem::create_directories(reportDirectory, createError);
        if (createError) {
            return std::unexpected(failure("EXPORT_CREATE_FAILED", "Unable to create the controlled report directory."));
        }

        struct ReportRow {
            std::string label;
            std::string context;
            std::string primaryText;
            std::string secondaryText;
            long double magnitude = 0;
            bool negative = false;
        };
        const auto formatSeconds = [](double seconds) {
            std::ostringstream value;
            value << std::fixed << std::setprecision(3);
            if (std::abs(seconds) >= 1.0) value << seconds << " s";
            else if (std::abs(seconds) >= 0.001) value << seconds * 1000.0 << " ms";
            else value << seconds * 1000000.0 << " us";
            return value.str();
        };
        const auto formatNanoseconds = [](std::int64_t nanoseconds) {
            std::ostringstream value;
            value << std::fixed << std::setprecision(3);
            const auto absolute = std::llabs(nanoseconds);
            if (absolute >= 1000000000) value << static_cast<double>(nanoseconds) / 1000000000.0 << " s";
            else if (absolute >= 1000000) value << static_cast<double>(nanoseconds) / 1000000.0 << " ms";
            else if (absolute >= 1000) value << static_cast<double>(nanoseconds) / 1000.0 << " us";
            else value << nanoseconds << " ns";
            return value.str();
        };
        const auto formatBytes = [](std::int64_t bytes) {
            std::ostringstream value;
            if (bytes > 0) value << '+';
            value << std::fixed << std::setprecision(2);
            const auto absolute = std::llabs(bytes);
            if (absolute >= 1024ll * 1024 * 1024) value << static_cast<double>(bytes) / (1024.0 * 1024 * 1024) << " GiB";
            else if (absolute >= 1024ll * 1024) value << static_cast<double>(bytes) / (1024.0 * 1024) << " MiB";
            else if (absolute >= 1024) value << static_cast<double>(bytes) / 1024.0 << " KiB";
            else value << bytes << " B";
            return value.str();
        };
        const auto markdownEscape = [](std::string value) {
            value = replaceToken(std::move(value), "|", "\\|");
            value = replaceToken(std::move(value), "\r", " ");
            return replaceToken(std::move(value), "\n", " ");
        };

        std::vector<ReportRow> rows;
        std::string title;
        std::string primaryHeading;
        std::string secondaryHeading;
        if (job->request.kind == ProfilerKind::PythonCpu) {
            title = "Python Performance Profile";
            primaryHeading = "Total";
            secondaryHeading = "Self";
            for (const auto& row : job->data.value("nodes", Json::array())) {
                if (!row.is_array() || row.size() < 11 || !row[6].is_number() || !row[7].is_number()) continue;
                const auto self = row[6].get<double>();
                const auto total = row[7].get<double>();
                const auto module = row[1].is_string() ? row[1].get<std::string>() : std::string{};
                const auto line = row[2].is_number_integer() ? row[2].get<std::int64_t>() : 0;
                const auto name = row[3].is_string() ? row[3].get<std::string>() : "unknown";
                const auto context = row[9].is_string() ? row[9].get<std::string>() : "Thread";
                const auto target = row[10].is_string() ? row[10].get<std::string>() : "unknown";
                rows.push_back({
                    .label = name,
                    .context = module + (line > 0 ? ":" + std::to_string(line) : "") + " | " + target + " / " + context,
                    .primaryText = formatSeconds(total),
                    .secondaryText = formatSeconds(self),
                    .magnitude = std::max(0.0, total),
                });
            }
        } else if (job->request.kind == ProfilerKind::PythonMemory) {
            title = "Python Memory Profile";
            primaryHeading = "Retained change";
            secondaryHeading = "Current retained";
            for (const auto& row : job->data.value("rows", Json::array())) {
                if (!row.is_array() || row.size() < 6 || !row[1].is_number_integer() || !row[3].is_number_integer()) continue;
                const auto difference = row[1].get<std::int64_t>();
                const auto current = row[3].get<std::int64_t>();
                std::string location = "unknown allocation site";
                if (row[5].is_array() && !row[5].empty() && row[5][0].is_array() && row[5][0].size() >= 2) {
                    const auto file = row[5][0][0].is_string() ? row[5][0][0].get<std::string>() : std::string{};
                    const auto line = row[5][0][1].is_number_integer() ? row[5][0][1].get<std::int64_t>() : 0;
                    location = file + (line > 0 ? ":" + std::to_string(line) : "");
                }
                const auto count = row[2].is_number_integer() ? row[2].get<std::int64_t>() : 0;
                rows.push_back({
                    .label = location,
                    .context = (count > 0 ? "+" : "") + std::to_string(count) + " blocks",
                    .primaryText = formatBytes(difference),
                    .secondaryText = formatBytes(current),
                    .magnitude = static_cast<long double>(std::llabs(difference)),
                    .negative = difference < 0,
                });
            }
        } else {
            title = "Native Performance Profile";
            primaryHeading = "Total";
            secondaryHeading = "Self";
            for (const auto& zone : job->data.value("zones", Json::array())) {
                if (!zone.is_object()) continue;
                const auto total = jsonInteger(zone, "totalNanoseconds");
                const auto self = jsonInteger(zone, "selfNanoseconds");
                const auto file = jsonString(zone, "sourceFile");
                const auto line = jsonInteger(zone, "sourceLine");
                const auto thread = jsonString(zone, "threadName", 512);
                rows.push_back({
                    .label = jsonString(zone, "name", 1024),
                    .context = (thread.empty() ? jsonString(zone, "threadId", 128) : thread) + " | "
                             + file + (line > 0 ? ":" + std::to_string(line) : ""),
                    .primaryText = formatNanoseconds(total),
                    .secondaryText = formatNanoseconds(self),
                    .magnitude = static_cast<long double>(std::max<std::int64_t>(0, total)),
                });
            }
        }
        std::stable_sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
            return left.magnitude > right.magnitude;
        });
        if (rows.size() > 40) rows.resize(40);

        std::ostringstream output;
        if (svg) {
            constexpr int width = 1400;
            constexpr int chartLeft = 540;
            constexpr int chartRight = 110;
            constexpr int rowHeight = 28;
            constexpr int top = 116;
            const auto height = std::max(250, top + static_cast<int>(rows.size()) * rowHeight + 54);
            const auto chartWidth = width - chartLeft - chartRight;
            const auto maximum = rows.empty() ? 1.0L : std::max(1.0L, rows.front().magnitude);
            output << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                   << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << width << "\" height=\"" << height
                   << "\" viewBox=\"0 0 " << width << ' ' << height << "\">\n"
                   << "<style>text{font-family:Segoe UI,Arial,sans-serif;letter-spacing:0}.title{font-size:20px;font-weight:600;fill:#f3f4f6}.meta,.value{font-size:12px;fill:#b9bec8}.label{font-size:12px;fill:#e5e7eb}g:hover rect{stroke:#fff;stroke-width:1}</style>\n"
                   << "<defs><clipPath id=\"label-clip\"><rect x=\"20\" y=\"90\" width=\"500\" height=\"" << height << "\"/></clipPath>"
                   << "<clipPath id=\"meta-clip\"><rect x=\"20\" y=\"0\" width=\"1360\" height=\"100\"/></clipPath></defs>\n"
                   << "<rect width=\"" << width << "\" height=\"" << height << "\" fill=\"#1e1f23\"/>\n"
                   << "<text x=\"20\" y=\"34\" class=\"title\">" << xmlEscape(title) << "</text>\n"
                   << "<text x=\"20\" y=\"58\" class=\"meta\">" << xmlEscape(job->snapshot.completedAt + " | " + job->snapshot.id) << "</text>\n"
                   << "<text x=\"20\" y=\"79\" class=\"meta\" clip-path=\"url(#meta-clip)\">" << xmlEscape(job->summary.dump()) << "</text>\n";
            for (std::size_t index = 0; index < rows.size(); ++index) {
                const auto& row = rows[index];
                const auto y = top + static_cast<int>(index) * rowHeight;
                const auto barWidth = std::max(1.0L, row.magnitude / maximum * chartWidth);
                const auto color = row.negative ? "#a855b5" : "#3b82f6";
                output << "<g><title>" << xmlEscape(row.label + "\n" + row.context + "\n" + primaryHeading + ": " + row.primaryText + "\n" + secondaryHeading + ": " + row.secondaryText) << "</title>"
                       << "<text x=\"20\" y=\"" << y + 17 << "\" class=\"label\" clip-path=\"url(#label-clip)\">" << xmlEscape(row.label + " | " + row.context) << "</text>"
                       << "<rect x=\"" << chartLeft << "\" y=\"" << y + 4 << "\" width=\"" << std::fixed << std::setprecision(2) << static_cast<double>(barWidth) << "\" height=\"18\" rx=\"3\" fill=\"" << color << "\"/>"
                       << "<text x=\"" << width - 18 << "\" y=\"" << y + 17 << "\" text-anchor=\"end\" class=\"value\">" << xmlEscape(row.primaryText) << "</text></g>\n";
            }
            if (rows.empty()) output << "<text x=\"20\" y=\"150\" class=\"meta\">No retained profiler records were captured.</text>\n";
            output << "</svg>\n";
        } else {
            output << "# " << title << "\n\n- Job: `" << job->snapshot.id << "`\n- Kind: `"
                   << toString(job->snapshot.kind) << "`\n- Storage: `" << toString(job->snapshot.storage)
                   << "`\n- Captured: `" << job->snapshot.completedAt << "`\n\n## Summary\n\n```json\n"
                   << job->summary.dump(2) << "\n```\n\n## Ranked Records\n\n| # | Entry | Context | "
                   << primaryHeading << " | " << secondaryHeading << " |\n| -: | --- | --- | ---: | ---: |\n";
            for (std::size_t index = 0; index < rows.size(); ++index) {
                output << "| " << index + 1 << " | " << markdownEscape(rows[index].label) << " | "
                       << markdownEscape(rows[index].context) << " | " << rows[index].primaryText << " | "
                       << rows[index].secondaryText << " |\n";
            }
            if (rows.empty()) output << "| 1 | No retained profiler records were captured. | | | |\n";
            output << "\n## Notes\n\n- The ranked table is intentionally bounded to 40 records.\n"
                   << "- Query the job for paged server-side detail before drawing conclusions from this report.\n";
            if (job->request.kind == ProfilerKind::PythonMemory) {
                output << "- Python memory values cover tracemalloc allocations, not process RSS or native memory.\n";
            } else if (job->request.kind == ProfilerKind::NativeCpu) {
                output << "- Native rows are emitted Tracy zones; uninstrumented native work may not appear.\n";
            }
        }
        if (!std::filesystem::is_regular_file(path)) {
            if (auto written = writeAtomic(path, output.str()); !written) return std::unexpected(written.error());
        }
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        auto digest = calculateFileSha256(path);
        if (!digest) return std::unexpected(digest.error());
        return ExportResult{
            .path = std::filesystem::absolute(path),
            .size = error ? 0 : size,
            .sha256 = std::move(*digest),
        };
    }

    std::expected<CleanupResult, ProfilerError> cleanup(const CleanupRequest& request) override {
        collectExpiredMemoryJobs();
        scanHistoryOnce();
        CleanupResult result;
        std::vector<std::filesystem::directory_entry> directories;
        std::error_code error;
        const auto appendDirectories = [&](const std::filesystem::path& root, bool skipInternal) {
            std::error_code iterationError;
            for (std::filesystem::directory_iterator it(root, iterationError), end;
                 !iterationError && it != end;
                 it.increment(iterationError)) {
                const auto filename = it->path().filename().string();
                if (!it->is_directory(iterationError) || it->is_symlink(iterationError)
                    || (skipInternal && !filename.empty() && filename.front() == '.')) {
                    continue;
                }
                directories.push_back(*it);
            }
        };
        appendDirectories(options_.storageRoot, true);
        appendDirectories(options_.storageRoot / ".exports", false);
        std::sort(directories.begin(), directories.end(), [](const auto& left, const auto& right) {
            std::error_code ignored;
            return left.last_write_time(ignored) > right.last_write_time(ignored);
        });
        constexpr std::uintmax_t MaximumRetainedBytes = 2ull * 1024 * 1024 * 1024;
        const auto oldestAllowed = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24 * 30);
        std::uintmax_t retainedBytes = 0;
        for (std::size_t index = 0; index < directories.size(); ++index) {
            std::uintmax_t bytes = 0;
            for (std::filesystem::recursive_directory_iterator it(directories[index].path(), error), end; !error && it != end; it.increment(error)) {
                if (it->is_regular_file(error)) bytes += it->file_size(error);
            }
            const bool expired = directories[index].last_write_time(error) < oldestAllowed;
            const bool overCount = index >= 50;
            const bool overBytes = retainedBytes + bytes > MaximumRetainedBytes;
            if (!expired && !overCount && !overBytes) {
                retainedBytes += bytes;
                continue;
            }
            ++result.removedJobs;
            result.removedBytes += bytes;
            if (!request.dryRun) std::filesystem::remove_all(directories[index].path(), error);
        }
        return result;
    }

    std::expected<Capabilities, ProfilerError> inspectCapabilities(const DoctorRequest& request) override {
        collectExpiredMemoryJobs();
        Capabilities result;
        const bool ipcAvailable = static_cast<bool>(options_.executeCode);
        const auto add = [&](ProfilerKind kind, bool available, std::string reason) {
            result.entries.push_back({kind, available, std::move(reason)});
        };
        if (!request.kind || *request.kind == ProfilerKind::PythonCpu) {
            add(ProfilerKind::PythonCpu, ipcAvailable, ipcAvailable ? "Game IPC executor is configured; Yappi availability is verified at start." : "Game IPC executor is unavailable.");
        }
        if (!request.kind || *request.kind == ProfilerKind::PythonMemory) {
            add(ProfilerKind::PythonMemory, ipcAvailable, ipcAvailable ? "Game IPC executor is configured; tracemalloc availability is verified at start." : "Game IPC executor is unavailable.");
        }
        if (!request.kind || *request.kind == ProfilerKind::NativeCpu) {
            if (request.deep) {
                const auto endpoint = native_.discover(options_.currentGameProcessId ? options_.currentGameProcessId() : 0);
                add(ProfilerKind::NativeCpu, endpoint.has_value(), endpoint ? "Verified DLL and one PID-owned Tracy endpoint." : endpoint.error().message);
                result.warnings.push_back("Native deep doctor performs one endpoint discovery. The verified DLL remains loaded until MCDK exits.");
            } else {
                add(ProfilerKind::NativeCpu, native_.available(), native_.availabilityReason());
            }
        }
        return result;
    }

    void shutdown() noexcept override {
        std::vector<std::shared_ptr<Job>> jobs;
        {
            std::lock_guard lock(mutex_);
            if (shuttingDown_.load(std::memory_order_acquire)) return;
            shuttingDown_.store(true, std::memory_order_release);
            for (auto& [id, job] : jobs_) {
                if (job->worker.joinable()) {
                    job->stopRequested = true;
                    if (job->nativeCapture.value) native_.stop(job->nativeCapture);
                    job->condition.notify_all();
                    jobs.push_back(job);
                }
            }
        }
        for (const auto& job : jobs) if (job->worker.joinable()) job->worker.join();
        native_.shutdown();
    }

private:
    std::expected<Json, ProfilerError> execute(std::string code, ProfileTarget side, std::chrono::milliseconds timeout) const {
        if (!options_.executeCode) return std::unexpected(failure("GAME_EXECUTOR_UNAVAILABLE", "Game IPC executor is unavailable.", true));
        auto value = options_.executeCode(std::move(code), side, timeout);
        if (!value) return std::unexpected(failure(value.error().code, value.error().message, value.error().retryable));
        return std::move(*value);
    }

    std::expected<void, ProfilerError> startBackend(Job& job) {
        if (job.request.kind == ProfilerKind::PythonCpu) {
            const auto side = job.request.target == ProfileTarget::Server ? ProfileTarget::Server : ProfileTarget::Client;
            auto started = execute(pythonCpuStartCode(job.request, job.snapshot.id), side, std::chrono::seconds(12));
            if (!started || !validProfilerPayload(*started)) {
                cleanupPython(job.request.kind, side, job.snapshot.id);
                return std::unexpected(started ? failure("PYTHON_PROFILER_START_FAILED", "Yappi is busy or could not start.", true) : started.error());
            }
            if (job.request.target == ProfileTarget::All) {
                const auto client = execute(pythonCpuMarkerCode(ProfileTarget::Client), ProfileTarget::Client, std::chrono::seconds(5));
                const auto server = execute(pythonCpuMarkerCode(ProfileTarget::Server), ProfileTarget::Server, std::chrono::seconds(5));
                if (!client || !server) {
                    cleanupPython(job.request.kind, ProfileTarget::Client, job.snapshot.id);
                    return std::unexpected(failure("PYTHON_PROFILER_MARKER_FAILED", "Client/server context markers could not both execute.", true));
                }
            }
            return {};
        }
        if (job.request.kind == ProfilerKind::PythonMemory) {
            auto started = execute(
                pythonMemoryStartCode(job.request, job.snapshot.id),
                ProfileTarget::Client,
                std::chrono::seconds(12)
            );
            if (!started || !validProfilerPayload(*started)) {
                cleanupPython(job.request.kind, ProfileTarget::Client, job.snapshot.id);
                return std::unexpected(started ? failure("PYTHON_MEMORY_START_FAILED", "tracemalloc is busy or could not start.", true) : started.error());
            }
            return {};
        }
        if (!native_.available()) {
            const auto retry = native_.initialize();
            if (!retry) return std::unexpected(retry.error());
        }
        const auto pid = options_.currentGameProcessId ? options_.currentGameProcessId() : 0;
        auto capture = native_.start(pid, job.request.duration, job.temporaryTrace);
        if (!capture) return std::unexpected(capture.error());
        job.nativeCapture = *capture;
        return {};
    }

    void cleanupPython(ProfilerKind kind, ProfileTarget side, std::string_view owner) const noexcept {
        try {
            (void)execute(
                kind == ProfilerKind::PythonMemory ? pythonMemoryCleanupCode(owner) : pythonCpuCleanupCode(owner),
                side,
                std::chrono::seconds(8)
            );
        } catch (...) {}
    }

    void runJob(const std::shared_ptr<Job>& job) noexcept {
        try {
            if (job->request.kind == ProfilerKind::NativeCpu) waitNative(job);
            else {
                std::unique_lock lock(mutex_);
                job->condition.wait_until(lock, job->deadline, [&] {
                    return job->stopRequested.load(std::memory_order_acquire)
                        || shuttingDown_.load(std::memory_order_acquire);
                });
                job->snapshot.state = JobState::Finalizing;
                job->snapshot.statusMessage = "Capture deadline reached; collecting bounded results.";
                lock.unlock();
                finalizePython(job);
            }
        } catch (const std::exception& error) {
            finishFailed(job, failure("PROFILER_WORKER_EXCEPTION", error.what()));
        } catch (...) {
            finishFailed(job, failure("PROFILER_WORKER_EXCEPTION", "Profiler worker raised an unknown exception."));
        }
    }

    void finalizePython(const std::shared_ptr<Job>& job) {
        if (job->discardRequested.load(std::memory_order_acquire)
            || shuttingDown_.load(std::memory_order_acquire)) {
            cleanupPython(
                job->request.kind,
                job->request.target == ProfileTarget::Server ? ProfileTarget::Server : ProfileTarget::Client,
                job->snapshot.id
            );
            finishDiscarded(
                job,
                shuttingDown_.load(std::memory_order_acquire) ? JobState::Aborted : JobState::Discarded
            );
            return;
        }
        const auto side = job->request.target == ProfileTarget::Server ? ProfileTarget::Server : ProfileTarget::Client;
        auto result = execute(
            job->request.kind == ProfilerKind::PythonMemory
                ? pythonMemoryCollectCode(job->request.collectGarbage, job->snapshot.id)
                : pythonCpuCollectCode(job->request, job->snapshot.id),
            side,
            std::chrono::seconds(30)
        );
        if (!result || !validProfilerPayload(*result)) {
            cleanupPython(job->request.kind, side, job->snapshot.id);
            finishFailed(job, result ? failure("PYTHON_COLLECT_FAILED", "Python profiler returned no owned capture.") : result.error());
            return;
        }
        job->data = std::move(*result);
        job->summary = summarize(*job);
        persistAndComplete(job);
    }

    void waitNative(const std::shared_ptr<Job>& job) {
        bool cleanupPending = false;
        while (true) {
            {
                std::unique_lock lock(mutex_);
                job->condition.wait_for(lock, std::chrono::milliseconds(100), [&] {
                    return job->stopRequested.load(std::memory_order_acquire)
                        || shuttingDown_.load(std::memory_order_acquire);
                });
                const bool processChanged = !options_.currentGameProcessId
                    || options_.currentGameProcessId() != job->nativeCapture.endpoint.pid
                    || !native_.validateProcess(job->nativeCapture.endpoint);
                if (job->stopRequested.load(std::memory_order_acquire)
                    || shuttingDown_.load(std::memory_order_acquire) || processChanged
                    || Clock::now() >= job->deadline) {
                    native_.stop(job->nativeCapture);
                }
            }
            const auto state = native_.status(job->nativeCapture);
            if (state == 4 || state == 5) break;
            if (!cleanupPending && Clock::now() >= job->deadline + std::chrono::seconds(15)) {
                std::lock_guard lock(mutex_);
                job->snapshot.state = JobState::CleanupPending;
                job->snapshot.partial = true;
                job->snapshot.statusMessage = "Native finalization exceeded its grace period; cooperative cleanup continues and the global lock remains held.";
                cleanupPending = true;
            } else if (!cleanupPending) {
                std::lock_guard lock(mutex_);
                job->snapshot.state = JobState::Finalizing;
            }
        }
        const auto state = native_.status(job->nativeCapture);
        if (state == 5) {
            const auto message = native_.error(job->nativeCapture);
            native_.release(job->nativeCapture);
            finishFailed(job, failure(cleanupPending ? "NATIVE_FINALIZE_TIMEOUT" : "NATIVE_CAPTURE_FAILED", message));
            return;
        }
        auto raw = native_.result(job->nativeCapture);
        native_.release(job->nativeCapture);
        if (cleanupPending) {
            finishFailed(
                job,
                failure(
                    "NATIVE_FINALIZE_TIMEOUT",
                    "Native finalization exceeded the completion grace period; cooperative cleanup eventually finished."
                )
            );
            return;
        }
        if (!raw) {
            finishFailed(job, raw.error());
            return;
        }
        auto parsed = Json::parse(*raw, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) {
            finishFailed(job, failure("NATIVE_RESULT_INVALID", "Native bridge returned invalid JSON."));
            return;
        }
        if (job->discardRequested.load(std::memory_order_acquire)
            || shuttingDown_.load(std::memory_order_acquire)) {
            finishDiscarded(
                job,
                shuttingDown_.load(std::memory_order_acquire) ? JobState::Aborted : JobState::Discarded
            );
            return;
        }
        parsed["gamePid"] = job->nativeCapture.endpoint.pid;
        parsed["tracyPort"] = job->nativeCapture.endpoint.port;
        parsed["processIdentity"] = job->nativeCapture.endpoint.identity;
        job->data = std::move(parsed);
        job->summary = summarize(*job);
        persistAndComplete(job);
    }

    Json summarize(const Job& job) const {
        Json result{{"kind", toString(job.request.kind)}, {"capture_truncated", job.data.value("truncated", false)}};
        if (job.request.kind == ProfilerKind::PythonCpu) {
            result["elapsed_seconds"] = job.data.value("elapsed", 0.0);
            result["total_functions"] = job.data.value("total", 0);
            result["captured_functions"] = job.data.value("nodes", Json::array()).size();
            result["captured_calls"] = job.data.value("edges", Json::array()).size();
        } else if (job.request.kind == ProfilerKind::PythonMemory) {
            result["elapsed_seconds"] = job.data.value("elapsed", 0.0);
            result["net_size_diff_bytes"] = job.data.value("sizeDiff", 0);
            result["net_count_diff"] = job.data.value("countDiff", 0);
            result["total_allocations"] = job.data.value("total", 0);
            result["captured_allocations"] = job.data.value("rows", Json::array()).size();
        } else {
            result["captured_seconds"] = job.data.value("capturedSeconds", 0.0);
            result["total_zones"] = job.data.value("totalZones", 0);
            result["indexed_zones"] = job.data.value("zones", Json::array()).size();
            result["call_tree_truncated"] = job.data.value("callTreeTruncated", false);
            result["thread_count"] = job.data.value("threads", Json::array()).size();
        }
        return result;
    }

    void persistAndComplete(const std::shared_ptr<Job>& job) {
        if (job->request.storage == ProfileStorage::Memory) {
            std::error_code ignored;
            std::filesystem::remove_all(job->temporaryTrace.parent_path(), ignored);
            const auto completedAt = utcNow();
            std::lock_guard lock(mutex_);
            job->snapshot.state = JobState::Completed;
            job->snapshot.completedAt = completedAt;
            job->snapshot.statusMessage =
                "Capture completed in temporary memory; it expires after 20 minutes without access.";
            job->lastAccess = monotonicNow();
            if (active_ == job) active_.reset();
            return;
        }
        {
            std::lock_guard lock(mutex_);
            job->snapshot.state = JobState::Persisting;
            job->snapshot.statusMessage = "Writing the bounded capture dataset to controlled storage.";
        }
        std::error_code error;
        if (std::filesystem::exists(job->directory, error)) {
            finishFailed(job, failure("PERSIST_CONFLICT", "The server-generated job directory already exists."));
            return;
        }
        std::filesystem::create_directories(job->directory, error);
        if (error) {
            finishFailed(job, failure("PERSIST_CREATE_FAILED", "Unable to create the controlled job directory."));
            return;
        }
        if (auto data = writeAtomic(job->directory / "data.json", job->data.dump()); !data) {
            finishFailed(job, data.error()); return;
        }
        if (auto summary = writeAtomic(job->directory / "summary.json", job->summary.dump(2)); !summary) {
            finishFailed(job, summary.error()); return;
        }
        if (job->request.kind == ProfilerKind::NativeCpu) {
            const auto target = job->directory / "capture.tracy";
            std::filesystem::copy_file(job->temporaryTrace, target, std::filesystem::copy_options::none, error);
            if (error || std::filesystem::file_size(target, error) == 0) {
                finishFailed(job, failure("PERSIST_TRACE_FAILED", "Unable to commit the Native trace artifact."));
                return;
            }
        }
        const auto completedAt = utcNow();
        Json manifest{
            {"schema", 1}, {"job_id", job->snapshot.id}, {"kind", toString(job->snapshot.kind)},
            {"storage", "disk"},
            {"state", "completed"}, {"created_at", job->snapshot.createdAt}, {"completed_at", completedAt},
            {"partial", job->snapshot.partial}, {"artifacts", Json::array({"data.json", "summary.json"})}
        };
        if (job->request.kind == ProfilerKind::NativeCpu) manifest["artifacts"].push_back("capture.tracy");
        if (auto committed = writeAtomic(job->directory / "manifest.json", manifest.dump(2)); !committed) {
            finishFailed(job, committed.error()); return;
        }
        std::filesystem::remove_all(job->temporaryTrace.parent_path(), error);
        {
            std::lock_guard lock(mutex_);
            job->snapshot.state = JobState::Completed;
            job->snapshot.completedAt = completedAt;
            job->snapshot.statusMessage = "Capture completed and committed to controlled storage.";
            job->lastAccess = monotonicNow();
            if (active_ == job) active_.reset();
        }
    }

    void finishFailed(const std::shared_ptr<Job>& job, const ProfilerError& error) {
        std::error_code ignored;
        std::filesystem::remove_all(job->temporaryTrace.parent_path(), ignored);
        std::lock_guard lock(mutex_);
        job->snapshot.state = JobState::Failed;
        job->snapshot.statusMessage = error.code + ": " + error.message;
        job->snapshot.completedAt = utcNow();
        job->lastAccess = monotonicNow();
        if (active_ == job) active_.reset();
    }

    void finishDiscarded(const std::shared_ptr<Job>& job, JobState state) {
        std::error_code ignored;
        std::filesystem::remove_all(job->temporaryTrace.parent_path(), ignored);
        if (!job->directory.empty()) std::filesystem::remove_all(job->directory, ignored);
        std::lock_guard lock(mutex_);
        job->snapshot.state = state;
        job->snapshot.completedAt = utcNow();
        job->lastAccess = monotonicNow();
        job->snapshot.statusMessage = state == JobState::Aborted ? "Capture was aborted during shutdown." : "Capture was discarded and artifacts were removed.";
        if (active_ == job) active_.reset();
    }

    std::shared_ptr<Job> findJob(const JobId& id) const {
        collectExpiredMemoryJobs();
        {
            std::lock_guard lock(mutex_);
            const auto found = jobs_.find(id);
            if (found != jobs_.end()) {
                found->second->lastAccess = monotonicNow();
                return found->second;
            }
        }
        if (id.empty() || id.size() > 128 || !std::all_of(id.begin(), id.end(), [](unsigned char value) {
                return std::isalnum(value) || value == '-';
            })) {
            return nullptr;
        }
        const auto directory = options_.storageRoot / id;
        std::error_code error;
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(directory, error))) return nullptr;
        std::ifstream manifestInput(directory / "manifest.json", std::ios::binary);
        std::ifstream dataInput(directory / "data.json", std::ios::binary);
        std::ifstream summaryInput(directory / "summary.json", std::ios::binary);
        auto manifest = Json::parse(manifestInput, nullptr, false);
        auto data = Json::parse(dataInput, nullptr, false);
        auto summary = Json::parse(summaryInput, nullptr, false);
        if (!manifest.is_object() || manifest.value("job_id", "") != id
            || manifest.value("state", "") != "completed" || !data.is_object() || !summary.is_object()) {
            return nullptr;
        }
        auto job = std::make_shared<Job>();
        job->snapshot.id = id;
        const auto kind = manifest.value("kind", "");
        job->snapshot.kind = kind == "python.memory" ? ProfilerKind::PythonMemory
                           : kind == "native.cpu" ? ProfilerKind::NativeCpu : ProfilerKind::PythonCpu;
        job->request.kind = job->snapshot.kind;
        job->request.storage = ProfileStorage::Disk;
        job->snapshot.storage = ProfileStorage::Disk;
        job->snapshot.state = JobState::Completed;
        job->snapshot.partial = manifest.value("partial", false);
        job->snapshot.createdAt = manifest.value("created_at", "");
        job->snapshot.completedAt = manifest.value("completed_at", "");
        job->snapshot.statusMessage = "Recovered committed profiler job.";
        job->directory = directory;
        job->data = std::move(data);
        job->summary = std::move(summary);
        job->lastAccess = monotonicNow();
        std::lock_guard lock(mutex_);
        const auto [found, inserted] = jobs_.emplace(id, job);
        found->second->lastAccess = monotonicNow();
        return inserted ? job : found->second;
    }

    [[nodiscard]] Clock::time_point monotonicNow() const {
        return options_.monotonicNow ? options_.monotonicNow() : Clock::now();
    }

    static bool isTerminal(JobState state) noexcept {
        return state == JobState::Completed || state == JobState::Failed || state == JobState::Discarded
            || state == JobState::Aborted;
    }

    void collectExpiredMemoryJobs() const {
        const auto now = monotonicNow();
        std::vector<std::shared_ptr<Job>> expired;
        {
            std::lock_guard lock(mutex_);
            for (auto it = jobs_.begin(); it != jobs_.end();) {
                const auto& job = it->second;
                const bool idleExpired = now >= job->lastAccess
                    && now - job->lastAccess >= options_.memoryIdleTimeout;
                if (job->request.storage == ProfileStorage::Memory && isTerminal(job->snapshot.state)
                    && active_ != job && idleExpired) {
                    expired.push_back(job);
                    it = jobs_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (const auto& job : expired) {
            if (job->worker.joinable() && job->worker.get_id() != std::this_thread::get_id()) {
                job->worker.join();
            }
        }
    }

    JobSnapshot snapshotOf(const std::shared_ptr<Job>& job) const {
        std::lock_guard lock(mutex_);
        return job->snapshot;
    }

    static std::string defaultSort(std::string_view view) {
        if (view == "allocations" || view == "growth" || view == "retained") return "size_diff";
        if (view == "threads") return "total_nanoseconds";
        return "total_time";
    }

    static std::string cursorSignature(const std::string& binding) {
        std::ostringstream output;
        output << std::hex << std::hash<std::string>{}(binding);
        return output.str();
    }

    std::expected<std::vector<QueryRecord>, ProfilerError> recordsFor(const Job& job, std::string_view view) const {
        std::vector<QueryRecord> records;
        if (job.request.kind == ProfilerKind::PythonCpu) {
            if (view == "hotspots" || view == "functions" || view == "contexts") {
                for (const auto& row : job.data.value("nodes", Json::array())) {
                    if (!row.is_array() || row.size() < 11) continue;
                    QueryRecord record{.id = "fn:" + std::to_string(row[0].get<std::int64_t>())};
                    addField(record, "module", row[1].get<std::string>());
                    addField(record, "line", row[2].get<std::int64_t>());
                    addField(record, "name", row[3].get<std::string>());
                    addField(record, "calls", row[4].get<std::int64_t>());
                    addField(record, "actual_calls", row[5].get<std::int64_t>());
                    addField(record, "self_time", row[6].get<double>(), "seconds");
                    addField(record, "total_time", row[7].get<double>(), "seconds");
                    addField(record, "context_id", row[8].get<std::int64_t>());
                    addField(record, "context_name", row[9].get<std::string>());
                    addField(record, "target", row[10].is_string() ? row[10].get<std::string>() : "unknown");
                    records.push_back(std::move(record));
                }
                return records;
            }
            if (view == "callers" || view == "callees") {
                for (const auto& row : job.data.value("edges", Json::array())) {
                    if (!row.is_array() || row.size() < 5) continue;
                    QueryRecord record{.id = "edge:" + std::to_string(records.size())};
                    addField(record, "caller_id", "fn:" + std::to_string(row[0].get<std::int64_t>()));
                    addField(record, "callee_id", "fn:" + std::to_string(row[1].get<std::int64_t>()));
                    addField(record, "calls", row[2].get<std::int64_t>());
                    addField(record, "self_time", row[3].get<double>(), "seconds");
                    addField(record, "total_time", row[4].get<double>(), "seconds");
                    records.push_back(std::move(record));
                }
                return records;
            }
        } else if (job.request.kind == ProfilerKind::PythonMemory) {
            if (view == "allocations" || view == "growth" || view == "retained" || view == "traceback") {
                for (const auto& row : job.data.value("rows", Json::array())) {
                    if (!row.is_array() || row.size() < 6) continue;
                    QueryRecord record{.id = "allocation:" + std::to_string(row[0].get<std::int64_t>())};
                    addField(record, "size_diff", row[1].get<std::int64_t>(), "bytes");
                    addField(record, "count_diff", row[2].get<std::int64_t>());
                    addField(record, "current_size", row[3].get<std::int64_t>(), "bytes");
                    addField(record, "current_count", row[4].get<std::int64_t>());
                    addField(record, "traceback", row[5].dump());
                    records.push_back(std::move(record));
                }
                return records;
            }
        } else {
            if (view == "hotspots" || view == "source-locations") {
                for (const auto& zone : job.data.value("zones", Json::array())) {
                    if (!zone.is_object()) continue;
                    QueryRecord record{.id = "zone:" + std::to_string(jsonInteger(zone, "id"))};
                    addField(record, "name", jsonString(zone, "name", 1024));
                    addField(record, "source_file", jsonString(zone, "sourceFile"));
                    addField(record, "source_line", jsonInteger(zone, "sourceLine"));
                    addField(record, "thread_id", jsonString(zone, "threadId", 128));
                    addField(record, "thread_name", jsonString(zone, "threadName", 512));
                    addField(record, "calls", jsonInteger(zone, "calls"));
                    addField(record, "total_time", jsonInteger(zone, "totalNanoseconds"), "nanoseconds");
                    addField(record, "self_time", jsonInteger(zone, "selfNanoseconds"), "nanoseconds");
                    addField(record, "mean_time", jsonInteger(zone, "meanNanoseconds"), "nanoseconds");
                    addField(record, "maximum_time", jsonInteger(zone, "maximumNanoseconds"), "nanoseconds");
                    records.push_back(std::move(record));
                }
                return records;
            }
            if (view == "threads") {
                for (const auto& thread : job.data.value("threads", Json::array())) {
                    QueryRecord record{.id = "thread:" + jsonString(thread, "id", 128)};
                    addField(record, "name", jsonString(thread, "name", 512));
                    addField(record, "calls", jsonInteger(thread, "calls"));
                    addField(record, "total_time", jsonInteger(thread, "totalNanoseconds"), "nanoseconds");
                    records.push_back(std::move(record));
                }
                return records;
            }
            if (view == "calltree-roots" || view == "calltree-children") {
                for (const auto& thread : job.data.value("threads", Json::array())) {
                    flattenNodes(thread.value("roots", Json::array()), jsonString(thread, "id", 128), "", 0, records);
                }
                if (view == "calltree-roots") {
                    std::erase_if(records, [](const auto& record) {
                        const auto parent = record.fields.find("parent_id");
                        return parent != record.fields.end() && !fieldText(parent->second).empty();
                    });
                }
                return records;
            }
        }
        return std::unexpected(failure("VIEW_INVALID", "The requested view is not available for this profiler kind."));
    }

    static void flattenNodes(const Json& nodes, const std::string& threadId, const std::string& parent, std::int64_t depth, std::vector<QueryRecord>& output) {
        if (!nodes.is_array()) return;
        for (const auto& node : nodes) {
            const auto id = "node:" + std::to_string(jsonInteger(node, "id"));
            QueryRecord record{.id = id};
            addField(record, "parent_id", parent);
            addField(record, "thread_id", threadId);
            addField(record, "depth", depth);
            addField(record, "name", jsonString(node, "name", 1024));
            addField(record, "source_file", jsonString(node, "sourceFile"));
            addField(record, "source_line", jsonInteger(node, "sourceLine"));
            addField(record, "calls", jsonInteger(node, "calls"));
            addField(record, "total_time", jsonInteger(node, "totalNanoseconds"), "nanoseconds");
            addField(record, "self_time", jsonInteger(node, "selfNanoseconds"), "nanoseconds");
            addField(record, "mean_time", jsonInteger(node, "meanNanoseconds"), "nanoseconds");
            addField(record, "maximum_time", jsonInteger(node, "maximumNanoseconds"), "nanoseconds");
            output.push_back(std::move(record));
            flattenNodes(node.value("children", Json::array()), threadId, id, depth + 1, output);
        }
    }

    void scanHistoryOnce() const {
        std::lock_guard lock(mutex_);
        if (historyScanned_) return;
        historyScanned_ = true;
        std::error_code error;
        for (std::filesystem::directory_iterator it(options_.storageRoot, error), end; !error && it != end; it.increment(error)) {
            if (!it->is_directory(error) || it->is_symlink(error)) continue;
            const auto manifestPath = it->path() / "manifest.json";
            std::ifstream input(manifestPath, std::ios::binary);
            Json manifest = Json::parse(input, nullptr, false);
            if (!manifest.is_object() || manifest.value("state", "") != "completed") continue;
            JobSnapshot snapshot;
            snapshot.id = manifest.value("job_id", "");
            const auto kind = manifest.value("kind", "");
            snapshot.kind = kind == "python.memory" ? ProfilerKind::PythonMemory
                          : kind == "native.cpu" ? ProfilerKind::NativeCpu : ProfilerKind::PythonCpu;
            snapshot.storage = ProfileStorage::Disk;
            snapshot.state = JobState::Completed;
            snapshot.partial = manifest.value("partial", false);
            snapshot.createdAt = manifest.value("created_at", "");
            snapshot.completedAt = manifest.value("completed_at", "");
            snapshot.statusMessage = "Recovered committed profiler job.";
            if (!snapshot.id.empty()) historyCache_.push_back(std::move(snapshot));
        }
    }

    ProfilerServiceOptions options_;
    NativeBridgeLoader native_;
    mutable std::mutex mutex_;
    mutable std::unordered_map<JobId, std::shared_ptr<Job>> jobs_;
    std::shared_ptr<Job> active_;
    mutable bool historyScanned_ = false;
    mutable std::vector<JobSnapshot> historyCache_;
    std::atomic<bool> shuttingDown_ = false;
};

std::expected<std::shared_ptr<ProfilerService>, ProfilerError>
createProfilerService(ProfilerServiceOptions options) {
    try {
        if (options.storageRoot.empty() || options.executableDirectory.empty()) {
            return std::unexpected(failure("PROFILER_CONFIGURATION_INVALID", "Profiler storage and executable roots are required."));
        }
        if (options.memoryIdleTimeout <= Clock::duration::zero()) {
            return std::unexpected(failure("PROFILER_CONFIGURATION_INVALID", "Memory idle timeout must be positive."));
        }
        return std::static_pointer_cast<ProfilerService>(std::make_shared<DefaultProfilerService>(std::move(options)));
    } catch (const std::exception& error) {
        return std::unexpected(failure("PROFILER_INITIALIZATION_FAILED", error.what(), true));
    } catch (...) {
        return std::unexpected(failure("PROFILER_INITIALIZATION_FAILED", "Unknown profiler initialization failure.", true));
    }
}

} // namespace mcdk::performance
