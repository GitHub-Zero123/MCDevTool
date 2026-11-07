#pragma once
// mcdk.game.process.create 的受理窗口：启动线程开窗、发事件、取结果，
// 插件在窗口内经 commit_process 交回自己创建的进程。契约见 04-events.md §4.4。
#include <cstdint>
#include <optional>
#include <string>

#include <mcdk/plugin/abi/core.h>

namespace mcdk::plugin_host {

    // 插件交回的进程。两个句柄是宿主自己打开的（HANDLE），所有权归取走它的人。
    struct CommittedProcess {
        std::string   ownerId;
        std::uint32_t pid     = 0;
        std::uint32_t tid     = 0;
        void*         process = nullptr;
        void*         thread  = nullptr;
    };

    // 同一时刻只能开一个。析构时若结果没被取走，就终止那个挂起的进程：
    // 宿主在交接途中失败，它不会再被恢复，留着只会成为永远挂起的孤儿。
    class ProcessRequest {
    public:
        ProcessRequest();
        ~ProcessRequest();

        ProcessRequest(const ProcessRequest&)            = delete;
        ProcessRequest& operator=(const ProcessRequest&) = delete;

        [[nodiscard]] mcdk_handle handle() const noexcept { return mHandle; }
        [[nodiscard]] bool        settled() const;

        // 取走后窗口即关闭，之后的 commit 一律 INVALID_HANDLE。
        [[nodiscard]] std::optional<CommittedProcess> take();

    private:
        mcdk_handle mHandle = 0;
    };

    // commit_process 的实现体，由 mcdk.game 的 shim 调用。
    [[nodiscard]] mcdk_status commitProcess(mcdk_handle owner, mcdk_handle request, std::uint32_t pid, std::uint32_t tid);

    // 插件句柄 → 插件 id，给错误信息点名用。句柄失效时返回空串。
    [[nodiscard]] std::string pluginIdOf(mcdk_handle plugin);

} // namespace mcdk::plugin_host
