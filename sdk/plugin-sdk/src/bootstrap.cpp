//
// SDK 的唯一一个 .cpp：错误槽与状态码描述。
//
// 其余部分都是 header-only，随插件一起编译，因此 SDK 与插件必然使用同一套
// 编译器、标准库与 CRT——这正是 SDK 内部可以自由使用 std::string 的前提。
//

#include <mcdk/plugin/detail/barrier.hpp>
#include <mcdk/plugin/error.hpp>

#include <utility>

namespace mcdk {

    std::string_view describeStatus(mcdk_status status) noexcept {
        switch (status) {
        case MCDK_OK:
            return "ok";
        case MCDK_ERR_INVALID_ARGUMENT:
            return "invalid argument";
        case MCDK_ERR_INVALID_HANDLE:
            return "invalid handle";
        case MCDK_ERR_NOT_SUPPORTED:
            return "not supported by this host version";
        case MCDK_ERR_WRONG_STAGE:
            return "called in the wrong lifecycle stage";
        case MCDK_ERR_OUT_OF_MEMORY:
            return "out of memory";
        case MCDK_ERR_TIMEOUT:
            return "timed out";
        case MCDK_ERR_GAME_NOT_READY:
            return "the game has not entered a world";
        case MCDK_ERR_DUPLICATE:
            return "already registered";
        case MCDK_ERR_BUFFER_TOO_SMALL:
            return "caller buffer too small";
        case MCDK_ERR_PLUGIN_EXCEPTION:
            return "exception escaped from plugin code";
        case MCDK_ERR_HOST_EXCEPTION:
            return "exception escaped from host code";
        default:
            return "unknown status";
        }
    }

} // namespace mcdk

namespace mcdk::detail {

    ErrorSlot& errorSlot() noexcept {
        // 按线程隔离：get_last_error 只返回当前线程上最近一次失败的信息。
        // 在工作线程上失败、到主线程上取，是取不到的（05-interfaces.md §3）。
        static thread_local ErrorSlot slot;
        return slot;
    }

    mcdk_status setError(mcdk_status code, std::string message) noexcept {
        ErrorSlot& slot = errorSlot();
        slot.code       = code;
        // 本函数总是在异常屏障内部被调用，自身绝不能再抛出——而字符串赋值会分配内存。
#if MCDK_SDK_HAS_EXCEPTIONS
        try {
            slot.message = std::move(message);
        } catch (...) {
            slot.message.clear();
        }
#else
        slot.message = std::move(message);
#endif
        return code;
    }

    void clearError() noexcept {
        ErrorSlot& slot = errorSlot();
        slot.code       = MCDK_OK;
        slot.message.clear();
    }

} // namespace mcdk::detail
