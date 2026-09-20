#include <mcdk/log_buffer.hpp>

#include <algorithm>
#include <utility>

namespace mcdk {

    LogBuffer::LogBuffer(std::size_t capacity, std::size_t clearBatchSize)
    : mCapacity(capacity),
      mClearBatchSize(clearBatchSize) {}

    void LogBuffer::add(std::string line) {
        std::lock_guard lock(mMutex);
        mBuffer.push_back(std::move(line));
        if (mBuffer.size() > mCapacity) {
            const auto count = std::min(mClearBatchSize, mBuffer.size());
            for (std::size_t index = 0; index < count; ++index) {
                mBuffer.pop_front();
            }
        }
    }

    void LogBuffer::clear() {
        std::lock_guard lock(mMutex);
        mBuffer.clear();
    }

    std::vector<std::string> LogBuffer::getLatest(std::size_t maxCount) {
        std::lock_guard lock(mMutex);
        if (maxCount >= mBuffer.size()) {
            // Keep deque as the internal eviction-friendly storage while preserving the vector-based public API.
            return std::vector<std::string>(mBuffer.begin(), mBuffer.end());
        }
        return std::vector<std::string>(
            mBuffer.end() - static_cast<std::ptrdiff_t>(maxCount),
            mBuffer.end()
        );
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
        return std::vector<std::string>(
            mBuffer.end() - static_cast<std::ptrdiff_t>(endIndex),
            mBuffer.end() - static_cast<std::ptrdiff_t>(index)
        );
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

    void LogBuffer::visitRange(
        std::size_t                                                       index,
        std::size_t                                                       endIndex,
        bool                                                              newestFirst,
        const std::function<void(std::size_t index, const std::string&)>& visitor
    ) {
        if (!visitor) {
            return;
        }
        std::lock_guard lock(mMutex);
        // 与 getRange 完全同一套边界判定，不得分化。
        if (mBuffer.empty() || index >= mBuffer.size() || endIndex > mBuffer.size() || index >= endIndex) {
            return;
        }
        const auto first = mBuffer.end() - static_cast<std::ptrdiff_t>(endIndex);
        const auto last  = mBuffer.end() - static_cast<std::ptrdiff_t>(index);
        if (newestFirst) {
            for (auto it = last; it != first;) {
                --it;
                visitor(static_cast<std::size_t>(mBuffer.end() - it) - 1, *it);
            }
        } else {
            for (auto it = first; it != last; ++it) {
                visitor(static_cast<std::size_t>(mBuffer.end() - it) - 1, *it);
            }
        }
    }

} // namespace mcdk
