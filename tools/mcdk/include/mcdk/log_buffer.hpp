#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcdk {

    class LogBuffer {
    public:
        struct Entry {
            std::uint64_t seq = 0;
            std::string   text;
        };

        struct Query {
            // 从这个序号起读（含）；为空时取最新的 limit 条。
            std::optional<std::uint64_t>             since;
            std::size_t                              limit = 50;
            std::function<bool(std::string_view)>    match;
        };

        struct Page {
            std::vector<Entry> entries;
            std::uint64_t      next    = 0; // 下次传给 since 的游标
            std::uint64_t      skipped = 0; // since 之后已被淘汰或清空、读不到的行数
            bool               more    = false;
        };

        explicit LogBuffer(std::size_t capacity = 1000, std::size_t clearBatchSize = 250);

        void add(std::string line);
        // 清空内容，序号继续递增，旧游标仍然有效。
        void clear();

        [[nodiscard]] Page query(const Query& query) const;

        [[nodiscard]] std::vector<std::string> getLatest(std::size_t maxCount);
        [[nodiscard]] std::vector<std::string> getLatestReversed(std::size_t maxCount);
        [[nodiscard]] std::vector<std::string> getRange(std::size_t index, std::size_t endIndex);
        [[nodiscard]] std::vector<std::string> getRangeReversed(std::size_t index, std::size_t endIndex);

        [[nodiscard]] std::size_t size() const;

    private:
        [[nodiscard]] std::uint64_t firstSeq() const noexcept;

        // A deque keeps batched front eviction O(k) without moving every retained log line.
        std::deque<Entry>  mBuffer;
        std::uint64_t      mNextSeq = 1;
        std::size_t        mCapacity;
        std::size_t        mClearBatchSize;
        mutable std::mutex mMutex;
    };

} // namespace mcdk
