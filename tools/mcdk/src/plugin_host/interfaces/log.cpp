//
// mcdk.log/1 的宿主实现。
//
// 本文件里的每个导出函数都必须经过 guard / guardVoid，没有例外
// （docs/plugin-system/02-abi-contract.md §4.3）。
//

#include <cstdint>
#include <memory>
#include <string>

#include <mcdk/log_buffer.hpp>
#include <mcdk/plugin/abi/iface/log.h>
#include <mcdk/plugin_host/guard.hpp>

#include "../registry.hpp"

namespace mcdk::plugin_host::detail {

    namespace {

        [[nodiscard]] std::shared_ptr<LogBuffer> bufferFor(mcdk_log_channel channel) {
            const auto binding = sessionBinding();
            switch (channel) {
            case MCDK_LOG_CHANNEL_STDERR:
                return binding.errBuffer;
            case MCDK_LOG_CHANNEL_STDOUT:
                return binding.logBuffer;
            default:
                return nullptr;
            }
        }

        mcdk_status MCDK_CALL logQuery(
            mcdk_handle           self,
            const mcdk_log_query* query,
            mcdk_log_sink         sink,
            void*                 user
        ) noexcept {
            return guard([&]() -> mcdk_status {
                if (query == nullptr || sink == nullptr) {
                    return MCDK_ERR_INVALID_ARGUMENT;
                }
                if (query->struct_size < sizeof(mcdk_log_query)) {
                    return MCDK_ERR_INVALID_ARGUMENT;
                }
                if (registry().find(self) == nullptr) {
                    return MCDK_ERR_INVALID_HANDLE;
                }
                if (query->channel != MCDK_LOG_CHANNEL_STDOUT && query->channel != MCDK_LOG_CHANNEL_STDERR) {
                    return MCDK_ERR_INVALID_ARGUMENT;
                }
                const auto buffer = bufferFor(query->channel);
                if (!buffer) {
                    // 运行期尚未绑定（REGISTER / CONFIG / WORLD 阶段），不是错误：
                    // 缓冲区还不存在，等价于「零条」。
                    return MCDK_OK;
                }

                const std::size_t total = buffer->size();
                const std::size_t start = query->start_index;
                std::size_t       end   = query->end_index;
                if (end <= start) {
                    // end == start 时改用 max_count 截断；max_count == 0 表示到缓冲区末端。
                    end = query->max_count == 0 ? total : start + query->max_count;
                }
                if (query->max_count != 0) {
                    end = std::min<std::size_t>(end, start + query->max_count);
                }
                end = std::min(end, total);

                const bool newestFirst = query->order != MCDK_LOG_ORDER_ASC;
                buffer->visitRange(start, end, newestFirst, [&](std::size_t index, const std::string& line) {
                    mcdk_log_entry entry{};
                    entry.struct_size = static_cast<std::uint32_t>(sizeof(entry));
                    entry.index       = static_cast<std::uint32_t>(index);
                    // LogBuffer 只存文本，不存时间戳，所以 v1 这里恒为 0。
                    // 需要时间戳的插件应订阅 mcdk.log.line —— 那条路径上有。
                    entry.timestamp_ms = 0;
                    entry.text.ptr     = line.data();
                    entry.text.len     = line.size();
                    // sink 是插件侧函数，异常由对方的 SDK 屏障吃掉；这里拿到的只会是返回。
                    sink(user, &entry);
                });
                return MCDK_OK;
            });
        }

        uint32_t MCDK_CALL logCount(mcdk_handle self, mcdk_log_channel channel) noexcept {
            std::uint32_t count = 0;
            guardVoid([&] {
                if (registry().find(self) == nullptr) {
                    return;
                }
                if (const auto buffer = bufferFor(channel)) {
                    count = static_cast<std::uint32_t>(buffer->size());
                }
            });
            return count;
        }

        constexpr mcdk_iface_log kTable = {
            /* struct_size */ static_cast<uint32_t>(sizeof(mcdk_iface_log)),
            /* _reserved   */ 0u,
            /* query       */ &logQuery,
            /* count       */ &logCount,
        };

    } // namespace

    const mcdk_iface_log* logTable() noexcept { return &kTable; }

} // namespace mcdk::plugin_host::detail
