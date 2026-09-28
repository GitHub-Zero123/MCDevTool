#include <mcdk/plugin_host/process_request.hpp>

#include <mcdk/plugin_host/guard.hpp>

#include "registry.hpp"

#include <mutex>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace mcdk::plugin_host {

    namespace {

        struct State {
            std::mutex                      mutex;
            mcdk_handle                     active = 0;
            mcdk_handle                     next   = 1;
            std::optional<CommittedProcess> committed;
        };

        State& state() {
            static State instance;
            return instance;
        }

        void closeHandles(CommittedProcess& process, bool terminate) noexcept {
#ifdef _WIN32
            if (process.process != nullptr) {
                if (terminate) {
                    TerminateProcess(process.process, static_cast<UINT>(-1));
                }
                CloseHandle(process.process);
            }
            if (process.thread != nullptr) {
                CloseHandle(process.thread);
            }
#else
            (void)terminate;
#endif
            process.process = nullptr;
            process.thread  = nullptr;
        }

    } // namespace

    ProcessRequest::ProcessRequest() {
        const std::lock_guard lock(state().mutex);
        if (state().active != 0) {
            throw std::logic_error("mcdk.game.process.create 的受理窗口不可重入");
        }
        mHandle         = state().next++;
        state().active  = mHandle;
        state().committed.reset();
    }

    ProcessRequest::~ProcessRequest() {
        const std::lock_guard lock(state().mutex);
        if (state().active != mHandle) {
            return; // 已经 take 过
        }
        if (state().committed) {
            closeHandles(*state().committed, true);
            state().committed.reset();
        }
        state().active = 0;
    }

    bool ProcessRequest::settled() const {
        const std::lock_guard lock(state().mutex);
        return state().active == mHandle && state().committed.has_value();
    }

    std::optional<CommittedProcess> ProcessRequest::take() {
        const std::lock_guard lock(state().mutex);
        if (state().active != mHandle) {
            return std::nullopt;
        }
        auto result = std::move(state().committed);
        state().committed.reset();
        state().active = 0;
        return result;
    }

    mcdk_status commitProcess(mcdk_handle owner, mcdk_handle request, std::uint32_t pid, std::uint32_t tid) {
        const auto* record = detail::registry().find(owner);
        if (record == nullptr) {
            return MCDK_ERR_INVALID_HANDLE;
        }
        std::string ownerId = record->id;

        const std::lock_guard lock(state().mutex);
        if (request == 0 || request != state().active) {
            return setError(MCDK_ERR_INVALID_HANDLE, "request 已失效：只能在 mcdk.game.process.create 的回调里交回");
        }
        if (state().committed) {
            return setError(MCDK_ERR_DUPLICATE, "游戏进程已由插件 " + state().committed->ownerId + " 交回");
        }
        if (pid == 0 || tid == 0) {
            return setError(MCDK_ERR_INVALID_ARGUMENT, "pid 与 tid 都不能为 0");
        }
#ifdef _WIN32
        CommittedProcess process;
        process.ownerId = std::move(ownerId);
        process.pid     = pid;
        process.tid     = tid;
        process.process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, pid);
        if (process.process == nullptr) {
            return setError(MCDK_ERR_INVALID_ARGUMENT, "无法打开 pid " + std::to_string(pid));
        }
        process.thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
        if (process.thread == nullptr || GetProcessIdOfThread(process.thread) != pid) {
            closeHandles(process, false);
            return setError(MCDK_ERR_INVALID_ARGUMENT, "tid " + std::to_string(tid) + " 不属于 pid " + std::to_string(pid));
        }
        state().committed = std::move(process);
        return MCDK_OK;
#else
        (void)ownerId;
        return MCDK_ERR_NOT_SUPPORTED;
#endif
    }

    std::string pluginIdOf(mcdk_handle plugin) {
        const auto* record = detail::registry().find(plugin);
        return record != nullptr ? record->id : std::string{};
    }

} // namespace mcdk::plugin_host
