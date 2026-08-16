#include "platform.hpp"

#include <iostream>
#include <stdexcept>
#include <vector>

#define _MCDEV_LOG_OUTPUT_ENDL "\n"

#ifdef MCDEV_LOG_FORCE_FLUSH_ENDL
#undef _MCDEV_LOG_OUTPUT_ENDL
#define _MCDEV_LOG_OUTPUT_ENDL std::endl
#endif

namespace mcdk::detail {

    std::filesystem::path currentExecutableDirectory() {
        std::vector<wchar_t> buffer(512);
        while (true) {
            const auto written = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (written == 0) {
                throw std::runtime_error("GetModuleFileNameW failed");
            }
            if (written < buffer.size() - 1) {
                return std::filesystem::path(std::wstring(buffer.data(), written)).parent_path();
            }
            buffer.resize(buffer.size() * 2);
        }
    }

    void debuggerAttachToProcess(DWORD processId, int port) {
        std::string command;
        command.reserve(48);
        command.append("mcdbg.exe --pid ");
        command.append(std::to_string(processId));
        command.append(" --port ");
        command.append(std::to_string(port));

        STARTUPINFOA startupInfo = {sizeof(startupInfo)};
        PROCESS_INFORMATION processInfo = {};
        if (!CreateProcessA(
                nullptr,
                command.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &startupInfo,
                &processInfo
            )) {
            std::cerr << "警告：无法启动mcdbg.exe附加调试器，请确保其在环境变量路径中。"
                      << _MCDEV_LOG_OUTPUT_ENDL;
            return;
        }

        UniqueHandle debuggerProcess(processInfo.hProcess);
        UniqueHandle debuggerThread(processInfo.hThread);
        std::cout << "调试器已启动，正在附加到进程PID：" << processId << " 端口：" << port << " ..."
                  << _MCDEV_LOG_OUTPUT_ENDL;
    }

    std::wstring convertUtf8ToUtf16(const std::string& utf8) {
        if (utf8.empty()) {
            return {};
        }
        const int wideLength = MultiByteToWideChar(
            CP_UTF8,
            0,
            utf8.data(),
            static_cast<int>(utf8.size()),
            nullptr,
            0
        );
        if (wideLength == 0) {
            throw std::runtime_error("Failed to convert UTF-8 to UTF-16.");
        }
        std::wstring utf16(wideLength, L'\0');
        MultiByteToWideChar(
            CP_UTF8,
            0,
            utf8.data(),
            static_cast<int>(utf8.size()),
            utf16.data(),
            wideLength
        );
        return utf16;
    }

} // namespace mcdk::detail
