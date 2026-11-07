#include <mcdk/plugin_host/guard.hpp>

#include <utility>

namespace mcdk::plugin_host {

    ErrorSlot& errorSlot() noexcept {
        static thread_local ErrorSlot slot;
        return slot;
    }

    mcdk_status setError(mcdk_status code, std::string message) noexcept {
        ErrorSlot& slot = errorSlot();
        slot.code       = code;
        // 本函数总在屏障内部被调用，自身绝不能再抛——而字符串赋值会分配内存。
        try {
            slot.message = std::move(message);
        } catch (...) {
            slot.message.clear();
        }
        return code;
    }

    void clearError() noexcept {
        ErrorSlot& slot = errorSlot();
        slot.code       = MCDK_OK;
        slot.message.clear();
    }

} // namespace mcdk::plugin_host
