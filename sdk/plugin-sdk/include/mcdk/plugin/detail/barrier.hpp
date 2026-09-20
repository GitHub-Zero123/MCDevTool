#pragma once

//
// 异常屏障。
//
// 插件作者的代码可以随意抛异常；凡是最终会被宿主直接调用的函数指针，都必须先
// 经过这里。异常在此被吃掉并转成 mcdk_status 或各回调约定的「不干预」取值，
// 绝不允许穿越 C ABI 进入宿主栈帧。
//
// 失败语义按回调逐个约定，见 docs/plugin-system/02-abi-contract.md §4.2 的表格。
// 其中最要命的一条：可否决事件的处理器抛异常必须等效为 CONTINUE 而非 VETO，
// 否则插件里的一个 bug 会让游戏起不来。
//

#include <string>
#include <type_traits>
#include <utility>

#include "../abi/core.h"
#include "../error.hpp"

#if MCDK_SDK_HAS_EXCEPTIONS
#include <exception>
#include <new>
#endif

namespace mcdk::detail {

    // 线程局部错误槽。位于插件自己的 DLL 内，不跨界：宿主取错误时拿到的是
    // 指向本槽的借用 mcdk_str，必须立即拷贝。
    struct ErrorSlot {
        mcdk_status code = MCDK_OK;
        std::string message;
    };

    [[nodiscard]] ErrorSlot& errorSlot() noexcept;

    mcdk_status setError(mcdk_status code, std::string message) noexcept;

    void clearError() noexcept;

    // 有返回值的回调。onError 是该回调约定的「出错时的等效行为」。
    template <class Fn>
    [[nodiscard]] auto guard(Fn&& fn, std::invoke_result_t<Fn> onError) noexcept -> std::invoke_result_t<Fn> {
#if MCDK_SDK_HAS_EXCEPTIONS
        try {
            return fn();
        } catch (const Error& error) {
            setError(error.status(), error.what());
        } catch (const std::bad_alloc&) {
            setError(MCDK_ERR_OUT_OF_MEMORY, "plugin: bad_alloc");
        } catch (const std::exception& error) {
            setError(MCDK_ERR_PLUGIN_EXCEPTION, error.what());
        } catch (...) {
            setError(MCDK_ERR_PLUGIN_EXCEPTION, "plugin: unknown exception");
        }
        return onError;
#else
        // -fno-exceptions 构建：屏障退化为直通。ABI 签名完全不变，宿主无需
        // 知道插件是否开启了异常。
        (void)onError;
        return fn();
#endif
    }

    // 无返回值的回调（on_stage / on_unload 等）。
    template <class Fn>
    void guardVoid(Fn&& fn) noexcept {
#if MCDK_SDK_HAS_EXCEPTIONS
        try {
            fn();
        } catch (const Error& error) {
            setError(error.status(), error.what());
        } catch (const std::bad_alloc&) {
            setError(MCDK_ERR_OUT_OF_MEMORY, "plugin: bad_alloc");
        } catch (const std::exception& error) {
            setError(MCDK_ERR_PLUGIN_EXCEPTION, error.what());
        } catch (...) {
            setError(MCDK_ERR_PLUGIN_EXCEPTION, "plugin: unknown exception");
        }
#else
        fn();
#endif
    }

} // namespace mcdk::detail
