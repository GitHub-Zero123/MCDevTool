#pragma once

#include "../modules/console.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/types.h>
#endif

namespace mcdk::platform {
    struct ProcessEnvironment {
#ifdef _WIN32
        std::wstring block;
#else
        std::vector<std::string> entries;
#endif
    };

    struct Process {
#ifdef _WIN32
        PROCESS_INFORMATION pi{};
        HANDLE              outRead  = NULL;
        HANDLE              outWrite = NULL;
        HANDLE              errRead  = NULL;
        HANDLE              errWrite = NULL;
#else
        pid_t pid      = -1;
        int   outRead  = -1;
        int   outWrite = -1;
        int   errRead  = -1;
        int   errWrite = -1;
#endif
    };

    enum class PipeKind {
        Stdout,
        Stderr
    };

    void setupConsole();

    void printColoredAtomic(const std::string& msg, ConsoleColor color);

    ProcessEnvironment buildEnvironmentWithOverride(std::string_view key, std::string_view value);

    bool startProcess(
        Process&                          process,
        const std::vector<std::string>&   args,
        const ProcessEnvironment*         environment,
        std::string&                      errorMessage
    );

    int getProcessId(const Process& process);

    std::thread startPipeReader(
        Process&                                  process,
        PipeKind                                  pipe,
        bool                                      filterPython,
        std::function<void(const std::string&)>   processLine
    );

    void attachDebuggerToProcess(int pid, int port);

    void waitProcess(Process& process);

    void closeProcess(Process& process);
} // namespace mcdk::platform
