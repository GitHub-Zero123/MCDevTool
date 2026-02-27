#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <mutex>

namespace mcdk {
    class LogBuffer {
    private:
        std::vector<std::string> buffer;
        size_t                   capacity;
        size_t                   clearBatchSize;
        std::mutex               mutex;

    public:
        LogBuffer(size_t capacity = 1000, size_t clearBatchSize = 250)
        : capacity(capacity),
          clearBatchSize(clearBatchSize) {}

        // 添加日志行到缓冲区
        void add(std::string line) {
            std::lock_guard<std::mutex> lock(mutex);
            buffer.push_back(std::move(line));
            if (buffer.size() > capacity) {
                buffer.erase(buffer.begin(), buffer.begin() + clearBatchSize);
            }
        }

        // 清理日志缓冲区
        void clear() {
            std::lock_guard<std::mutex> lock(mutex);
            buffer.clear();
        }

        // 获取最大数量的最新日志
        std::vector<std::string> getLatest(size_t maxCount) {
            std::lock_guard<std::mutex> lock(mutex);
            if (maxCount >= buffer.size()) {
                return buffer;
            }
            return std::vector<std::string>(buffer.end() - maxCount, buffer.end());
        }

        // 获取最大数量的最新日志并反转顺序（最新的在前）
        std::vector<std::string> getLatestReversed(size_t maxCount) {
            std::lock_guard<std::mutex> lock(mutex);

            if (buffer.empty()) return {};

            size_t                   count = std::min(maxCount, buffer.size());
            std::vector<std::string> result(buffer.end() - count, buffer.end());

            // 反转顺序
            std::reverse(result.begin(), result.end());

            return result;
        }
    };
} // namespace mcdk