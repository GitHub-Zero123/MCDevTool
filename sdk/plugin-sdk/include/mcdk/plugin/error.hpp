#pragma once

#include <string>
#include <string_view>
#include <utility>

#include "abi/core.h"

#ifndef MCDK_SDK_HAS_EXCEPTIONS
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
#define MCDK_SDK_HAS_EXCEPTIONS 1
#else
#define MCDK_SDK_HAS_EXCEPTIONS 0
#endif
#endif

#if MCDK_SDK_HAS_EXCEPTIONS
#include <exception>
#endif

namespace mcdk {

    [[nodiscard]] std::string_view describeStatus(mcdk_status status) noexcept;

#if MCDK_SDK_HAS_EXCEPTIONS
    // 插件侧的异常类型。它只在插件自己的二进制内流动：SDK 的屏障会在每个交给
    // 宿主的回调边界上把它吃掉，转成 mcdk_status（02-abi-contract.md §4.2）。
    class Error : public std::exception {
    public:
        Error(mcdk_status status, std::string message) : mStatus(status), mMessage(std::move(message)) {}

        [[nodiscard]] mcdk_status status() const noexcept { return mStatus; }
        [[nodiscard]] const char* what() const noexcept override { return mMessage.c_str(); }

    private:
        mcdk_status mStatus;
        std::string mMessage;
    };
#else
    // 关闭异常时保留同名类型，使返回 std::expected 的 tryXxx 系列签名不变。
    class Error {
    public:
        Error(mcdk_status status, std::string message) : mStatus(status), mMessage(std::move(message)) {}

        [[nodiscard]] mcdk_status status() const noexcept { return mStatus; }
        [[nodiscard]] const char* what() const noexcept { return mMessage.c_str(); }

    private:
        mcdk_status mStatus;
        std::string mMessage;
    };
#endif

} // namespace mcdk
