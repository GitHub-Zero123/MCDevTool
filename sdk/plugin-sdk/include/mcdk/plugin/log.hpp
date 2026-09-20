#pragma once

//
// mcdk.log 的 C++ 封装。
//
// ABI 上是回调式枚举：宿主持着 LogBuffer 的锁逐条回调，文本是借用的。那套约束
// （sink 里禁止调用任何其他 mcdk 接口、禁止阻塞）很容易踩，所以 SDK 不把裸 sink
// 交给用户 —— 默认的 query() 在 sink 里只做一次 string 拷贝，返回一个普通的
// std::vector<LogEntry>。想要零拷贝的人可以用 visit()，代价是要自己遵守约束。
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

        // 零拷贝版本。visitor 运行在宿主持有 LogBuffer 锁的状态下：
        //
        //   - 必须极短，只做拷贝或匹配；
        //   - 禁止调用任何其他 mcdk 接口，包括 console()，会死锁；
        //   - 禁止阻塞或等待其他线程。
        //
        // 拿不准就用 query()。entry.text 在 visitor 返回后即失效。
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
            // 异常绝不能穿过 C ABI 回到宿主——宿主此刻还持着 LogBuffer 的锁。
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
