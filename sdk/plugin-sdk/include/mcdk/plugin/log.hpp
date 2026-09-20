#pragma once

//
// mcdk.log 的 C++ 封装。
//
// ABI 上是回调式枚举，文本是借用的：这样边界上就不会出现分配。宿主侧已经
// 改成「锁内取快照、锁外回调」，所以 visit() 的 visitor 并不跑在 LogBuffer 的锁里，
// 里面调其他接口也不会死锁。唯一的约束是 entry.text 返回后即失效。
//
// 默认的 query() 把它拷成 std::vector<LogEntry>，绝大多数场景用这个就行。
//

#include <cstdint>
#include <string>
#include <vector>

#include "abi/iface/log.h"
#include "detail/abi_bridge.hpp"
#include "detail/barrier.hpp"

namespace mcdk {

    enum class LogChannel {
        Stdout,
        Stderr
    };

    enum class LogOrder {
        NewestFirst,
        OldestFirst
    };

    struct LogEntry {
        // 相对最新条目的索引：0 是最新一条。
        std::uint32_t index = 0;
        // v1 恒为 0：LogBuffer 只存文本。需要时间戳请订阅 ev::LogLine。
        std::int64_t timestampMs = 0;
        std::string  text;
    };

    struct LogQuery {
        LogChannel channel = LogChannel::Stdout;
        // 含。0 = 最新一条。
        std::uint32_t startIndex = 0;
        // 不含。等于 startIndex 时改用 maxCount 截断。
        std::uint32_t endIndex = 0;
        // 0 = 不限，仍受缓冲区容量约束。
        std::uint32_t maxCount = 0;
        LogOrder      order    = LogOrder::NewestFirst;
    };

    class Log {
    public:
        Log() = default;

        Log(mcdk_handle self, const mcdk_iface_log* table) noexcept : mSelf(self), mTable(table) {}

        [[nodiscard]] bool available() const noexcept { return mTable != nullptr; }

        // 取回匹配的日志行。文本已拷贝，可以随便存。
        [[nodiscard]] std::vector<LogEntry> query(const LogQuery& request) const {
            std::vector<LogEntry> result;
            visit(request, [&result](const mcdk_log_entry& entry) {
                result.push_back(
                    LogEntry{entry.index, entry.timestamp_ms, std::string(detail::toView(entry.text))}
                );
            });
            return result;
        }

        // 取最近 count 行，最新的在前。
        [[nodiscard]] std::vector<LogEntry> latest(std::uint32_t count, LogChannel channel = LogChannel::Stdout)
            const {
            LogQuery request;
            request.channel  = channel;
            request.maxCount = count;
            request.order    = LogOrder::NewestFirst;
            return query(request);
        }

        [[nodiscard]] std::uint32_t count(LogChannel channel = LogChannel::Stdout) const noexcept {
            if (!detail::ifaceHas(mTable, &mcdk_iface_log::count)) {
                return 0;
            }
            return mTable->count(mSelf, toAbi(channel));
        }

        // 零拷贝版本。visitor 跑在调用线程上、宿主的锁外，里面调其他接口是安全的。
        //
        // **entry.text 在 visitor 返回后即失效**，要留必须拷走。拿不准就用 query()。
        template <class Visitor>
        void visit(const LogQuery& request, Visitor&& visitor) const {
            if (!detail::ifaceHas(mTable, &mcdk_iface_log::query)) {
                return;
            }
            mcdk_log_query raw{};
            raw.struct_size = static_cast<std::uint32_t>(sizeof(raw));
            raw.channel     = toAbi(request.channel);
            raw.start_index = request.startIndex;
            raw.end_index   = request.endIndex;
            raw.max_count   = request.maxCount;
            raw.order       = request.order == LogOrder::OldestFirst ? MCDK_LOG_ORDER_ASC : MCDK_LOG_ORDER_DESC;

            auto* sink = &visitor;
            mTable->query(mSelf, &raw, &trampoline<std::decay_t<Visitor>>, sink);
        }

    private:
        template <class Visitor>
        static void MCDK_CALL trampoline(void* user, const mcdk_log_entry* entry) {
            if (user == nullptr || entry == nullptr) {
                return;
            }
            // 异常绝不能穿过 C ABI 回到宿主的栈帧。
            detail::guardVoid([&] { (*static_cast<Visitor*>(user))(*entry); });
        }

        [[nodiscard]] static constexpr mcdk_log_channel toAbi(LogChannel channel) noexcept {
            // 显式映射而非 static_cast：ABI 枚举值永久冻结，SDK 侧枚举可以随便改。
            switch (channel) {
            case LogChannel::Stderr:
                return MCDK_LOG_CHANNEL_STDERR;
            case LogChannel::Stdout:
            default:
                return MCDK_LOG_CHANNEL_STDOUT;
            }
        }

        mcdk_handle           mSelf  = 0;
        const mcdk_iface_log* mTable = nullptr;
    };

} // namespace mcdk
