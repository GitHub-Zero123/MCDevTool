#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace mcdk {

    class LogBuffer {
    public:
        explicit LogBuffer(std::size_t capacity = 1000, std::size_t clearBatchSize = 250);

        void add(std::string line);
        void clear();

        [[nodiscard]] std::vector<std::string> getLatest(std::size_t maxCount);
        [[nodiscard]] std::vector<std::string> getLatestReversed(std::size_t maxCount);
        [[nodiscard]] std::vector<std::string> getRange(std::size_t index, std::size_t endIndex);
        [[nodiscard]] std::vector<std::string> getRangeReversed(std::size_t index, std::size_t endIndex);

    private:
        // A deque keeps batched front eviction O(k) without moving every retained log line.
        std::deque<std::string>  mBuffer;
        std::size_t              mCapacity;
        std::size_t              mClearBatchSize;
        std::mutex               mMutex;
    };

} // namespace mcdk
