#include "runtime.hpp"

#include <iostream>
#include <mutex>
#include <stdexcept>

#ifdef _WIN32
#include <string>
#else
#include <cerrno>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace {
    static std::mutex g_consoleMutex;

    void processBufferAppend(
        std::string&                                   lineBuf,
        const char*                                    buf,
        size_t                                         len,
        bool                                           filterPython,
        const std::function<void(const std::string&)>& processLine
    ) {
        lineBuf.append(buf, len);

        size_t pos = 0;
        while ((pos = lineBuf.find('\n')) != std::string::npos) {
            std::string line = lineBuf.substr(0, pos);
            lineBuf.erase(0, pos + 1);

            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (filterPython && line.find("[Python] ") == std::string::npos) continue;

            processLine(line);
        }
    }

    std::string quoteCommandArg(const std::string& arg) {
        if (arg.find_first_of(" \t\"") == std::string::npos) {
            return arg;
        }

        std::string quoted = "\"";
        for (char ch : arg) {
            if (ch == '"') {
                quoted += "\\\"";
            } else {
                quoted += ch;
            }
        }
        quoted += '"';
        return quoted;
    }

    std::string buildWindowsCommandLine(const std::vector<std::string>& args) {
        std::string cmd;
        for (size_t i = 0; i < args.size(); ++i) {
            if (i != 0) {
                cmd.push_back(' ');
            }
            cmd += quoteCommandArg(args[i]);
        }
        return cmd;
    }

#ifdef _WIN32
    std::wstring convertUtf8ToUtf16(const std::string& utf8Str) {
        if (utf8Str.empty()) {
            return std::wstring();
        }
        int wideCharLen = MultiByteToWideChar(CP_UTF8, 0, utf8Str.data(), static_cast<int>(utf8Str.size()), nullptr, 0);
        if (wideCharLen == 0) {
            throw std::runtime_error("Failed to convert UTF-8 to UTF-16.");
        }
        std::wstring utf16Str(wideCharLen, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8Str.data(), static_cast<int>(utf8Str.size()), &utf16Str[0], wideCharLen);
        return utf16Str;
    }

    std::wstring createNewEnvironmentBlock(const std::wstring& newVar, const std::wstring& newValue) {
        LPWCH envBlock = GetEnvironmentStringsW();
        if (envBlock == nullptr) {
            throw std::runtime_error("Failed to get current environment strings.");
        }

        std::wstring newEnvBlock;
        LPWCH        current = envBlock;
        while (*current) {
            std::wstring varLine(current);
            newEnvBlock += varLine + L'\0';
            current     += varLine.size() + 1;
        }

        newEnvBlock += newVar + L'=' + newValue + L'\0';
        newEnvBlock += L'\0';

        FreeEnvironmentStringsW(envBlock);
        return newEnvBlock;
    }

    void readPipeThread(HANDLE hPipe, bool filterPython, const std::function<void(const std::string&)>& processLine) {
        constexpr DWORD   BUFSZ = 4096;
        std::string       lineBuf;
        std::vector<char> buffer(BUFSZ);

        while (true) {
            DWORD bytesRead = 0;
            BOOL  ok        = ReadFile(hPipe, buffer.data(), BUFSZ, &bytesRead, NULL);
            if (!ok) {
                DWORD err = GetLastError();
                if (err == ERROR_BROKEN_PIPE) {
                    if (!lineBuf.empty()) {
                        std::string lastLine = lineBuf;
                        if (!lastLine.empty() && lastLine.back() == '\r') lastLine.pop_back();
                        if (!(filterPython && lastLine.find("[Python] ") == std::string::npos)) processLine(lastLine);
                        lineBuf.clear();
                    }
                    break;
                }
                break;
            }

            if (bytesRead == 0) {
                if (!lineBuf.empty()) {
                    std::string lastLine = lineBuf;
                    if (!lastLine.empty() && lastLine.back() == '\r') lastLine.pop_back();
                    if (!(filterPython && lastLine.find("[Python] ") == std::string::npos)) processLine(lastLine);
                    lineBuf.clear();
                }
                break;
            }

            processBufferAppend(lineBuf, buffer.data(), bytesRead, filterPython, processLine);
        }
    }
#else
    void readPipeThread(int fd, bool filterPython, const std::function<void(const std::string&)>& processLine) {
        constexpr size_t  BUFSZ = 4096;
        std::string       lineBuf;
        std::vector<char> buffer(BUFSZ);

        while (true) {
            ssize_t bytesRead = read(fd, buffer.data(), buffer.size());
            if (bytesRead < 0) {
                if (errno == EINTR) {
                    continue;
                }
                break;
            }

            if (bytesRead == 0) {
                if (!lineBuf.empty()) {
                    std::string lastLine = lineBuf;
                    if (!lastLine.empty() && lastLine.back() == '\r') lastLine.pop_back();
                    if (!(filterPython && lastLine.find("[Python] ") == std::string::npos)) processLine(lastLine);
                    lineBuf.clear();
                }
                break;
            }

            processBufferAppend(lineBuf, buffer.data(), static_cast<size_t>(bytesRead), filterPython, processLine);
        }
    }
#endif
} // namespace

namespace mcdk::platform {
    void setupConsole() {
#ifdef _WIN32
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
#endif
    }

    void printColoredAtomic(const std::string& msg, ConsoleColor color) {
        std::lock_guard<std::mutex> lk(g_consoleMutex);
#ifdef _WIN32
        HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);

        if (hConsole == INVALID_HANDLE_VALUE) {
            std::cout << msg << "\n";
            return;
        }

        CONSOLE_SCREEN_BUFFER_INFO info;
        if (!GetConsoleScreenBufferInfo(hConsole, &info)) {
            std::cout << msg << "\n";
            return;
        }

        WORD attr = 0;

        switch (color) {
        case ConsoleColor::Green:
            attr = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
            break;
        case ConsoleColor::Red:
            attr = FOREGROUND_RED | FOREGROUND_INTENSITY;
            break;
        case ConsoleColor::Blue:
            attr = FOREGROUND_BLUE | FOREGROUND_INTENSITY;
            break;
        case ConsoleColor::Yellow:
            attr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
            break;
        case ConsoleColor::Cyan:
            attr = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
            break;
        case ConsoleColor::Magenta:
            attr = FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
            break;
        case ConsoleColor::White:
            attr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
            break;
        case ConsoleColor::Black:
            attr = 0;
            break;
        case ConsoleColor::Gray:
            attr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
            break;
        case ConsoleColor::DarkGray:
            attr = FOREGROUND_INTENSITY;
            break;
        default:
            break;
        }

        if (color != ConsoleColor::Default) {
            SetConsoleTextAttribute(hConsole, attr);
        }

        std::cout << msg << "\n";

        if (color != ConsoleColor::Default) {
            SetConsoleTextAttribute(hConsole, info.wAttributes);
        }
#else
        const char* ansiCode = "";
        switch (color) {
        case ConsoleColor::Green:
            ansiCode = "\033[92m";
            break;
        case ConsoleColor::Red:
            ansiCode = "\033[91m";
            break;
        case ConsoleColor::Blue:
            ansiCode = "\033[94m";
            break;
        case ConsoleColor::Yellow:
            ansiCode = "\033[93m";
            break;
        case ConsoleColor::Cyan:
            ansiCode = "\033[96m";
            break;
        case ConsoleColor::Magenta:
            ansiCode = "\033[95m";
            break;
        case ConsoleColor::White:
            ansiCode = "\033[97m";
            break;
        case ConsoleColor::Black:
            ansiCode = "\033[30m";
            break;
        case ConsoleColor::Gray:
            ansiCode = "\033[37m";
            break;
        case ConsoleColor::DarkGray:
            ansiCode = "\033[90m";
            break;
        case ConsoleColor::Default:
        default:
            break;
        }

        if (color == ConsoleColor::Default) {
            std::cout << msg << "\n";
        } else {
            std::cout << ansiCode << msg << "\033[0m\n";
        }
#endif
    }

    ProcessEnvironment buildEnvironmentWithOverride(std::string_view key, std::string_view value) {
        ProcessEnvironment env;
#ifdef _WIN32
        env.block = createNewEnvironmentBlock(convertUtf8ToUtf16(std::string(key)), convertUtf8ToUtf16(std::string(value)));
#else
        std::string prefix = std::string(key) + "=";
        for (char** current = environ; current && *current; ++current) {
            std::string entry(*current);
            if (entry.rfind(prefix, 0) != 0) {
                env.entries.push_back(std::move(entry));
            }
        }
        env.entries.push_back(prefix + std::string(value));
#endif
        return env;
    }

    bool startProcess(
        Process&                        process,
        const std::vector<std::string>& args,
        const ProcessEnvironment*       environment,
        std::string&                    errorMessage
    ) {
#ifdef _WIN32
        STARTUPINFOW si = {sizeof(si)};

        SECURITY_ATTRIBUTES sa{};
        sa.nLength              = sizeof(sa);
        sa.bInheritHandle       = TRUE;
        sa.lpSecurityDescriptor = nullptr;

        if (!CreatePipe(&process.outRead, &process.outWrite, &sa, 0)) {
            errorMessage = "CreatePipe(stdout) failed";
            return false;
        }
        if (!SetHandleInformation(process.outRead, HANDLE_FLAG_INHERIT, 0)) {
            errorMessage = "SetHandleInformation(stdout) failed";
            return false;
        }
        if (!CreatePipe(&process.errRead, &process.errWrite, &sa, 0)) {
            errorMessage = "CreatePipe(stderr) failed";
            return false;
        }
        if (!SetHandleInformation(process.errRead, HANDLE_FLAG_INHERIT, 0)) {
            errorMessage = "SetHandleInformation(stderr) failed";
            return false;
        }

        si.dwFlags    |= STARTF_USESTDHANDLES;
        si.hStdOutput  = process.outWrite;
        si.hStdError   = process.errWrite;
        si.hStdInput   = GetStdHandle(STD_INPUT_HANDLE);

        auto commandLine    = buildWindowsCommandLine(args);
        auto mutableCommand = convertUtf8ToUtf16(commandLine);
        void* envBlock      = environment ? (void*)environment->block.data() : nullptr;

        if (!CreateProcessW(
                nullptr,
                mutableCommand.data(),
                nullptr,
                nullptr,
                TRUE,
                (envBlock != nullptr ? CREATE_UNICODE_ENVIRONMENT : 0),
                envBlock,
                nullptr,
                &si,
                &process.pi
            )) {
            errorMessage = "CreateProcessW failed";
            return false;
        }

        CloseHandle(process.outWrite);
        CloseHandle(process.errWrite);
        process.outWrite = NULL;
        process.errWrite = NULL;
        return true;
#else
        int outPipe[2] = {-1, -1};
        int errPipe[2] = {-1, -1};
        if (pipe(outPipe) != 0) {
            errorMessage = "pipe(stdout) failed: " + std::string(strerror(errno));
            return false;
        }
        if (pipe(errPipe) != 0) {
            close(outPipe[0]);
            close(outPipe[1]);
            errorMessage = "pipe(stderr) failed: " + std::string(strerror(errno));
            return false;
        }

        pid_t pid = fork();
        if (pid < 0) {
            close(outPipe[0]);
            close(outPipe[1]);
            close(errPipe[0]);
            close(errPipe[1]);
            errorMessage = "fork failed: " + std::string(strerror(errno));
            return false;
        }

        if (pid == 0) {
            dup2(outPipe[1], STDOUT_FILENO);
            dup2(errPipe[1], STDERR_FILENO);
            close(outPipe[0]);
            close(outPipe[1]);
            close(errPipe[0]);
            close(errPipe[1]);

            std::vector<char*> argv;
            argv.reserve(args.size() + 1);
            for (const auto& arg : args) {
                argv.push_back(const_cast<char*>(arg.c_str()));
            }
            argv.push_back(nullptr);

            if (environment) {
                std::vector<char*> envp;
                envp.reserve(environment->entries.size() + 1);
                for (const auto& entry : environment->entries) {
                    envp.push_back(const_cast<char*>(entry.c_str()));
                }
                envp.push_back(nullptr);
                execve(argv[0], argv.data(), envp.data());
            } else {
                execv(argv[0], argv.data());
            }
            _exit(127);
        }

        close(outPipe[1]);
        close(errPipe[1]);
        process.pid     = pid;
        process.outRead = outPipe[0];
        process.errRead = errPipe[0];
        return true;
#endif
    }

    int getProcessId(const Process& process) {
#ifdef _WIN32
        return static_cast<int>(process.pi.dwProcessId);
#else
        return static_cast<int>(process.pid);
#endif
    }

    std::thread startPipeReader(
        Process&                                process,
        PipeKind                                pipe,
        bool                                    filterPython,
        std::function<void(const std::string&)> processLine
    ) {
#ifdef _WIN32
        HANDLE handle = pipe == PipeKind::Stdout ? process.outRead : process.errRead;
#else
        int handle = pipe == PipeKind::Stdout ? process.outRead : process.errRead;
#endif
        return std::thread(readPipeThread, handle, filterPython, std::move(processLine));
    }

    void attachDebuggerToProcess(int pid, int port) {
#ifdef _WIN32
        std::string cmd;
        cmd.reserve(48);

        cmd.append("mcdbg.exe --pid ");
        cmd.append(std::to_string(pid));
        cmd.append(" --port ");
        cmd.append(std::to_string(port));
        STARTUPINFOA        si = {sizeof(si)};
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            std::cerr << "警告：无法启动mcdbg.exe附加调试器，请确保其在环境变量路径中。\n";
            return;
        }
        std::cout << "调试器已启动，正在附加到进程PID：" << pid << " 端口：" << port << " ...\n";
#else
        (void)pid;
        (void)port;
        std::cerr << "Warning: mcdbg debugger attach is only available on Windows.\n";
#endif
    }

    void waitProcess(Process& process) {
#ifdef _WIN32
        WaitForSingleObject(process.pi.hProcess, INFINITE);
#else
        if (process.pid > 0) {
            int status = 0;
            while (waitpid(process.pid, &status, 0) < 0 && errno == EINTR) {}
        }
#endif
    }

    void closeProcess(Process& process) {
#ifdef _WIN32
        if (process.outRead) CloseHandle(process.outRead);
        if (process.errRead) CloseHandle(process.errRead);
        if (process.outWrite) CloseHandle(process.outWrite);
        if (process.errWrite) CloseHandle(process.errWrite);
        if (process.pi.hProcess) CloseHandle(process.pi.hProcess);
        if (process.pi.hThread) CloseHandle(process.pi.hThread);
#else
        if (process.outRead >= 0) close(process.outRead);
        if (process.errRead >= 0) close(process.errRead);
        if (process.outWrite >= 0) close(process.outWrite);
        if (process.errWrite >= 0) close(process.errWrite);
#endif
    }
} // namespace mcdk::platform
