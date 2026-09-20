#pragma once
// 宿主侧的异常屏障。
// 与 SDK 侧的 detail::guard 对称：宿主现有代码大量使用异常（startGame 在游戏
#include <exception>
#include <new>
#include <string>
#include <type_traits>
#include <utility>

#include <mcdk/plugin/abi/core.h>

namespace mcdk::plugin_host {
// 线程局部错误槽。插件通过 mcdk.core 的 get_last_error 取走，取到的是指向
// 本槽的借用 mcdk_str，必须立即拷贝。按线程隔离——在工作线程上失败、到主
    struct ErrorSlot {
        mcdk_status code = MCDK_OK;
        std::string message;
    };

    [[nodiscard]] ErrorSlot& errorSlot() noexcept;

    mcdk_status setError(mcdk_status code, std::string message) noexcept;

    void clearError() noexcept;

    // 有返回值的 shim。fn 必须返回 mcdk_status。
    template <class Fn>
    [[nodiscard]] mcdk_status guard(Fn&& fn) noexcept {
        static_assert(
            std::is_same_v<std::invoke_result_t<Fn>, mcdk_status>,
            "host::guard requires the callable to return mcdk_status"
        );
        try {
            clearError();
            return fn();
        } catch (const std::bad_alloc&) {
            return setError(MCDK_ERR_OUT_OF_MEMORY, "host: bad_alloc");
        } catch (const std::exception& error) {
            // std::runtime_error / filesystem_error / nlohmann::json::exception
            // 都落在这里。
            return setError(MCDK_ERR_HOST_EXCEPTION, error.what());
        } catch (...) {
            return setError(MCDK_ERR_HOST_EXCEPTION, "host: unknown exception");
        }
    }

     // 无返回值的 shim（如 console log）。失败只记录，不向插件报告。
    // 这类调用往往本身就在错误处理路径上，返回值只会诱导出无意义的嵌套处理。
    template <class Fn>
    void guardVoid(Fn&& fn) noexcept {
        try {
            fn();
        } catch (const std::bad_alloc&) {
            setError(MCDK_ERR_OUT_OF_MEMORY, "host: bad_alloc");
        } catch (const std::exception& error) {
            setError(MCDK_ERR_HOST_EXCEPTION, error.what());
        } catch (...) {
            setError(MCDK_ERR_HOST_EXCEPTION, "host: unknown exception");
        }
    }

} // namespace mcdk::plugin_host
