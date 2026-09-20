#include <mcdk/runtime/mcp_tool_registry.hpp>

#include <utility>

namespace mcdk::runtime {

    std::string_view describeMcpToolBindError(McpToolBindError error) noexcept {
        switch (error) {
        case McpToolBindError::InvalidName:
            return "tool name must be non-empty";
        case McpToolBindError::DuplicateName:
            return "a tool with the same name is already registered";
        case McpToolBindError::EmptyHandler:
            return "tool handler must not be empty";
        case McpToolBindError::RegistrySealed:
            return "the tool registry is sealed; registration window has closed";
        }
        return "unknown tool bind error";
    }

    std::expected<void, McpToolBindError>
    McpToolRegistry::bind(mcp::tool descriptor, McpToolHandler handler, std::string owner) {
        if (descriptor.name.empty()) {
            return std::unexpected(McpToolBindError::InvalidName);
        }
        if (!handler) {
            return std::unexpected(McpToolBindError::EmptyHandler);
        }
        // 先查封存标志再加锁：封存之后不应再有写入者争抢这把锁。
        if (mSealed.load(std::memory_order_acquire)) {
            return std::unexpected(McpToolBindError::RegistrySealed);
        }

        const std::lock_guard lock(mMutex);
        // 加锁后复查：封存与注册可能来自不同线程。
        if (mSealed.load(std::memory_order_relaxed)) {
            return std::unexpected(McpToolBindError::RegistrySealed);
        }
        if (mIndex.contains(descriptor.name)) {
            return std::unexpected(McpToolBindError::DuplicateName);
        }

        auto name = descriptor.name;
        mEntries.push_back(
            McpToolEntry{
                .descriptor = std::move(descriptor),
                .handler    = std::move(handler),
                .owner      = std::move(owner),
            }
        );
        mIndex.emplace(std::move(name), mEntries.size() - 1);
        return {};
    }

    void McpToolRegistry::seal() { mSealed.store(true, std::memory_order_release); }

    bool McpToolRegistry::sealed() const noexcept { return mSealed.load(std::memory_order_acquire); }

    const McpToolEntry* McpToolRegistry::find(std::string_view name) const {
        const std::lock_guard lock(mMutex);
        const auto            found = mIndex.find(name);
        if (found == mIndex.end()) {
            return nullptr;
        }
        // mEntries 是 deque，追加不会让已发出的条目指针失效。
        return &mEntries[found->second];
    }

    std::size_t McpToolRegistry::size() const {
        const std::lock_guard lock(mMutex);
        return mEntries.size();
    }

    void McpToolRegistry::forEach(const std::function<void(const McpToolEntry&)>& visitor) const {
        if (!visitor) {
            return;
        }
        const std::lock_guard lock(mMutex);
        for (const auto& entry : mEntries) {
            visitor(entry);
        }
    }

} // namespace mcdk::runtime
