#include <log_buffer.hpp>

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
            mBuffer.erase(mBuffer.begin(), mBuffer.begin() + static_cast<std::ptrdiff_t>(count));
        }
    }

    void LogBuffer::clear() {
        std::lock_guard lock(mMutex);
        mBuffer.clear();
    }

    std::vector<std::string> LogBuffer::getLatest(std::size_t maxCount) {
        std::lock_guard lock(mMutex);
        if (maxCount >= mBuffer.size()) {
            return mBuffer;
        }
        return {mBuffer.end() - static_cast<std::ptrdiff_t>(maxCount), mBuffer.end()};
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
        return {
            mBuffer.end() - static_cast<std::ptrdiff_t>(endIndex),
            mBuffer.end() - static_cast<std::ptrdiff_t>(index),
        };
    }

    std::vector<std::string> LogBuffer::getRangeReversed(std::size_t index, std::size_t endIndex) {
        auto result = getRange(index, endIndex);
        std::ranges::reverse(result);
        return result;
    }

} // namespace mcdk
