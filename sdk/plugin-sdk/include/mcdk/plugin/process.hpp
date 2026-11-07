#pragma once
// 接管 ev::GameProcessCreate 用的 Win32 辅助。契约（挂起创建、继承句柄、用宿主给的
// std 句柄、只挂起一次）见 04-events.md §4.4；走这里的函数就不用自己记。
#ifdef _WIN32

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "events.hpp"
#include "game.hpp"

namespace mcdk::process {

    // 按长度转换，内嵌的 \0 原样保留。
    [[nodiscard]] inline std::wstring widen(std::string_view utf8) {
        if (utf8.empty()) {
            return {};
        }
        const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring wide(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
        return wide;
    }

    // 宿主的启动请求，UTF-16 形态。改完交给 createSuspended / launch。
    struct LaunchSpec {
        std::wstring commandLine;
        std::wstring environment; // 双 \0 结尾的环境块
        HANDLE       stdInput   = nullptr;
        HANDLE       stdOutput  = nullptr;
        HANDLE       stdError   = nullptr;
        DWORD        extraFlags = 0; // 叠加在 CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT 之上

        [[nodiscard]] static LaunchSpec from(const ev::GameProcessCreate& event) {
            LaunchSpec spec;
            spec.commandLine = widen(event.commandLine);
            spec.environment = widen(event.environment);
            spec.stdInput    = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(event.stdInput));
            spec.stdOutput   = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(event.stdOutput));
            spec.stdError    = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(event.stdError));
            return spec;
        }

        // 设置或覆盖一个变量，名字不分大小写。环境块要求按名字排序，这里顺带重排。
        void setEnvironment(std::wstring_view name, std::wstring_view value) {
            std::vector<std::wstring> entries;
            for (std::size_t begin = 0; begin < environment.size();) {
                const auto end = environment.find(L'\0', begin);
                if (end == std::wstring::npos || end == begin) {
                    break;
                }
                entries.emplace_back(environment, begin, end - begin);
                begin = end + 1;
            }
            entries.erase(
                std::remove_if(
                    entries.begin(),
                    entries.end(),
                    [name](const std::wstring& entry) { return sameName(nameOf(entry), name); }
                ),
                entries.end()
            );
            entries.push_back(std::wstring(name) + L'=' + std::wstring(value));
            std::sort(entries.begin(), entries.end(), [](const std::wstring& left, const std::wstring& right) {
                return CompareStringOrdinal(
                           left.data(),
                           static_cast<int>(left.size()),
                           right.data(),
                           static_cast<int>(right.size()),
                           TRUE
                       )
                    == CSTR_LESS_THAN;
            });
            environment.clear();
            for (const auto& entry : entries) {
                environment += entry;
                environment += L'\0';
            }
            environment += L'\0';
        }

    private:
        // "=C:=C:\\" 这类条目的名字以 '=' 开头，所以从第二个字符起找分隔符。
        [[nodiscard]] static std::wstring_view nameOf(std::wstring_view entry) {
            const auto separator = entry.find(L'=', 1);
            return separator == std::wstring_view::npos ? entry : entry.substr(0, separator);
        }

        [[nodiscard]] static bool sameName(std::wstring_view left, std::wstring_view right) {
            return CompareStringOrdinal(
                       left.data(),
                       static_cast<int>(left.size()),
                       right.data(),
                       static_cast<int>(right.size()),
                       TRUE
                   )
                == CSTR_EQUAL;
        }
    };

    // 按契约创建进程。成功时 out 的两个句柄归调用方；失败返回 false，原因看 GetLastError。
    [[nodiscard]] inline bool createSuspended(LaunchSpec& spec, PROCESS_INFORMATION& out) {
        STARTUPINFOW startup{};
        startup.cb         = sizeof(startup);
        startup.dwFlags    = STARTF_USESTDHANDLES;
        startup.hStdInput  = spec.stdInput;
        startup.hStdOutput = spec.stdOutput;
        startup.hStdError  = spec.stdError;
        out                = PROCESS_INFORMATION{};
        return CreateProcessW(
                   nullptr,
                   spec.commandLine.data(),
                   nullptr,
                   nullptr,
                   TRUE,
                   CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | spec.extraFlags,
                   spec.environment.empty() ? nullptr : spec.environment.data(),
                   nullptr,
                   &startup,
                   &out
               )
            != FALSE;
    }

    // 一步到位：创建、交回、关掉自己的句柄。返回值直接作为回调结果。
    // 交回失败时进程已经起了，这里把它终止，不留一个永远挂起的孤儿。
    [[nodiscard]] inline EventResult launch(const Game& game, const ev::GameProcessCreate& event, LaunchSpec spec) {
        PROCESS_INFORMATION info{};
        if (!createSuspended(spec, info)) {
            return EventResult::Veto;
        }
        const auto status = game.commitProcess(event.request, info.dwProcessId, info.dwThreadId);
        if (status != MCDK_OK) {
            TerminateProcess(info.hProcess, static_cast<UINT>(-1));
        }
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
        return status == MCDK_OK ? EventResult::Stop : EventResult::Veto;
    }

} // namespace mcdk::process

#endif // _WIN32
