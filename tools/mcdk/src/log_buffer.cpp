#include <mcdk/log_buffer.hpp>

#include <algorithm>
#include <utility>

namespace mcdk {

    LogBuffer::LogBuffer(std::size_t capacity, std::size_t clearBatchSize)
    : mCapacity(capacity),
      mClearBatchSize(clearBatchSize) {}

    void LogBuffer::add(std::string line) {
        std::lock_guard lock(mMutex);
        mBuffer.push_back({mNextSeq++, std::move(line)});
        if (mBuffer.size() > mCapacity) {
            const auto count = std::min(mClearBatchSize, mBuffer.size());
            mBuffer.erase(mBuffer.begin(), mBuffer.begin() + static_cast<std::ptrdiff_t>(count));
        }
    }

    void LogBuffer::clear() {
        std::lock_guard lock(mMutex);
        mBuffer.clear();
    }

    std::uint64_t LogBuffer::firstSeq() const noexcept { return mBuffer.empty() ? mNextSeq : mBuffer.front().seq; }

    LogBuffer::Page LogBuffer::query(const Query& query) const {
        std::lock_guard lock(mMutex);
        Page            page;
        const auto      matches = [&](const Entry& entry) { return !query.match || query.match(entry.text); };

        if (!query.since) {
            // 从新往旧收集，再翻回时间顺序。
            for (auto it = mBuffer.rbegin(); it != mBuffer.rend() && page.entries.size() < query.limit; ++it) {
                if (matches(*it)) {
                    page.entries.push_back(*it);
                }
            }
            std::ranges::reverse(page.entries);
            page.next = mNextSeq;
            return page;
        }

        // 游标超过当前序号说明来自上一次运行，从头读。
        const auto first = firstSeq();
        const auto since = *query.since > mNextSeq ? first : *query.since;
        page.skipped     = since < first ? first - since : 0;
        page.next        = mNextSeq;
        const auto begin = std::ranges::lower_bound(mBuffer, since, {}, &Entry::seq);
        for (auto it = begin; it != mBuffer.end(); ++it) {
            if (!matches(*it)) {
                continue;
            }
            if (page.entries.size() == query.limit) {
                page.more = true;
                page.next = it->seq;
                break;
            }
            page.entries.push_back(*it);
        }
        return page;
    }

    std::vector<std::string> LogBuffer::getLatest(std::size_t maxCount) {
        std::lock_guard lock(mMutex);
        const auto      count = std::min(maxCount, mBuffer.size());
        std::vector<std::string> result;
        result.reserve(count);
        for (auto it = mBuffer.end() - static_cast<std::ptrdiff_t>(count); it != mBuffer.end(); ++it) {
            result.push_back(it->text);
        }
        return result;
    }

    std::vector<std::string> LogBuffer::getLatestReversed(std::size_t maxCount) {
        auto result = getLatest(maxCount);
        std::ranges::reverse(result);
        return result;
    }

    std::vector<std::string> LogBuffer::getRange(std::size_t index, std::size_t endIndex) {
        std::lock_guard lock(mMutex);
        if (mBuffer.empty() || index >= mBuffer.size() || endIndex > mBuffer.size() || index >= endIndex) {
            return {};
        }
        std::vector<std::string> result;
        result.reserve(endIndex - index);
        for (auto it = mBuffer.end() - static_cast<std::ptrdiff_t>(endIndex);
             it != mBuffer.end() - static_cast<std::ptrdiff_t>(index);
             ++it) {
            result.push_back(it->text);
        }
        return result;
    }

    std::vector<std::string> LogBuffer::getRangeReversed(std::size_t index, std::size_t endIndex) {
        auto result = getRange(index, endIndex);
        std::ranges::reverse(result);
        return result;
    }

    std::size_t LogBuffer::size() const {
        std::lock_guard lock(mMutex);
        return mBuffer.size();
    }

} // namespace mcdk
