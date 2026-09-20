#pragma once

#include <cstddef>
#include <deque>
#include <functional>
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

        [[nodiscard]] std::size_t size() const;

        // 在持锁状态下逐条访问 [index, endIndex) 区间（索引 0 = 最新一条）。
        //
        // 与 getRange 的区别是不拷贝：visitor 拿到的是内部存储的引用，返回后即失效。
        // 这是 mcdk.log 接口的底层：插件侧拿到的是借用的 mcdk_str，不产生任何
        // 按条分配（docs/plugin-system/05-interfaces.md §7.1）。
        //
        // visitor 运行在锁内，因此必须极短，且禁止在其中调用本对象的其他方法。
        void visitRange(
            std::size_t                                                      index,
            std::size_t                                                      endIndex,
            bool                                                             newestFirst,
            const std::function<void(std::size_t index, const std::string&)>& visitor
        );

    private:
        // A deque keeps batched front eviction O(k) without moving every retained log line.
        std::deque<std::string>  mBuffer;
        std::size_t              mCapacity;
        std::size_t              mClearBatchSize;
        mutable std::mutex       mMutex;
    };

} // namespace mcdk
