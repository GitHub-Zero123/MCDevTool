#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace mcdk::detail {

    class UniqueHandle {
    public:
        UniqueHandle() = default;
        explicit UniqueHandle(HANDLE handle) noexcept : mHandle(handle) {}
        ~UniqueHandle() { reset(); }

        UniqueHandle(const UniqueHandle&)            = delete;
        UniqueHandle& operator=(const UniqueHandle&) = delete;

        UniqueHandle(UniqueHandle&& other) noexcept : mHandle(other.release()) {}

        UniqueHandle& operator=(UniqueHandle&& other) noexcept {
            if (this != &other) {
                reset(other.release());
            }
            return *this;
        }

        [[nodiscard]] HANDLE get() const noexcept { return mHandle; }

        HANDLE* receive() noexcept {
            reset();
            return &mHandle;
        }

        HANDLE release() noexcept {
            const HANDLE handle = mHandle;
            mHandle             = nullptr;
            return handle;
        }

        void reset(HANDLE handle = nullptr) noexcept {
            if (mHandle != nullptr && mHandle != INVALID_HANDLE_VALUE) {
                CloseHandle(mHandle);
            }
            mHandle = handle;
        }

    private:
        HANDLE mHandle = nullptr;
    };

    [[nodiscard]] std::filesystem::path currentExecutableDirectory();
    void debuggerAttachToProcess(DWORD processId, int port);
    [[nodiscard]] std::wstring convertUtf8ToUtf16(const std::string& utf8);
    // 按长度转换，内嵌的 \0 原样保留（环境块要用）。
    [[nodiscard]] std::string convertUtf16ToUtf8(std::wstring_view utf16);

} // namespace mcdk::detail
