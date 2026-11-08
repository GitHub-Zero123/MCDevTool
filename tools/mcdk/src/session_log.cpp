#include <mcdk/session_log.hpp>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

#ifdef _WIN32
namespace mcdk {

    struct RotatingOutputCapture::State {
        std::filesystem::path path;
        std::uintmax_t        maxBytes = 0;
        std::thread           writer;
        HANDLE                readEnd = nullptr;
    };

    namespace {
        void writeLoop(HANDLE readEnd, const std::filesystem::path& path, std::uintmax_t maxBytes) {
            auto           rotated = path;
            rotated += ".1";
            std::ofstream  file(path, std::ios::binary | std::ios::trunc);
            std::uintmax_t written = 0;
            char           buffer[16 * 1024];
            DWORD          count = 0;
            while (ReadFile(readEnd, buffer, sizeof(buffer), &count, nullptr) && count > 0) {
                if (written + count > maxBytes && written > 0) {
                    file.close();
                    std::error_code ignored;
                    std::filesystem::rename(path, rotated, ignored);
                    file.open(path, std::ios::binary | std::ios::trunc);
                    written = 0;
                }
                file.write(buffer, count);
                file.flush();
                written += count;
            }
        }

        // 把 CRT 的 fd 和 Win32 标准句柄都指向管道写端。dup 出的句柄不让子进程继承，
        // 否则游戏会拿着写端，mcdk 退出时等不到管道关闭。
        void redirect(int fd, DWORD stdHandle, HANDLE writeEnd) {
            const int pipeFd = _open_osfhandle(reinterpret_cast<intptr_t>(writeEnd), _O_BINARY);
            if (pipeFd < 0 || _dup2(pipeFd, fd) != 0) {
                throw std::runtime_error("无法重定向输出到会话日志");
            }
            _close(pipeFd);
            const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
            SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);
            SetStdHandle(stdHandle, handle);
        }
    } // namespace

    RotatingOutputCapture::RotatingOutputCapture(std::filesystem::path path, std::uintmax_t maxBytes)
    : mState(std::make_unique<State>()) {
        mState->path     = std::move(path);
        mState->maxBytes = maxBytes;
        std::filesystem::create_directories(mState->path.parent_path());

        HANDLE readEnd  = nullptr;
        HANDLE writeEnd = nullptr;
        if (!CreatePipe(&readEnd, &writeEnd, nullptr, 0)) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "CreatePipe");
        }
        HANDLE errorEnd = nullptr;
        DuplicateHandle(GetCurrentProcess(), writeEnd, GetCurrentProcess(), &errorEnd, 0, FALSE, DUPLICATE_SAME_ACCESS);
        mState->readEnd = readEnd;
        mState->writer  = std::thread(writeLoop, readEnd, mState->path, maxBytes);

        std::fflush(stdout);
        std::fflush(stderr);
        redirect(1, STD_OUTPUT_HANDLE, writeEnd);
        redirect(2, STD_ERROR_HANDLE, errorEnd);
    }

    RotatingOutputCapture::~RotatingOutputCapture() {
        std::cout.flush();
        std::cerr.flush();
        std::fflush(stdout);
        std::fflush(stderr);
        // 把 fd 1/2 换成 NUL，管道写端随之全部关闭，写线程读到 EOF 后退出。
        const int nul = _open("NUL", _O_WRONLY);
        if (nul >= 0) {
            _dup2(nul, 1);
            _dup2(nul, 2);
            _close(nul);
            SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(1)));
            SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(2)));
        }
        if (mState->writer.joinable()) {
            mState->writer.join();
        }
        CloseHandle(mState->readEnd);
    }

} // namespace mcdk
#endif
