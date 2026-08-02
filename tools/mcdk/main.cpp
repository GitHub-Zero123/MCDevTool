#include <application.hpp>
#include <config.hpp>
#include <console_output.hpp>
#include <env.hpp>

#include <cstdio>
#include <exception>
#include <iostream>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifdef MCDK_ENABLE_CLI
#ifdef _WIN32
int MCDK_CLI_PARSE(int argc, wchar_t* argv[]);
#else
int MCDK_CLI_PARSE(int argc, char* argv[]);
#endif
#endif

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#else
int main(int argc, char* argv[]) {
#endif
    if (mcdk::getEnvOutputMode() == 1) {
        setvbuf(stdout, nullptr, _IONBF, 0);
    }

#ifdef NDEBUG
    try {
#endif
#ifdef MCDK_ENABLE_CLI
        if (argc > 1) {
            return MCDK_CLI_PARSE(argc, argv);
        }
#endif
        mcdk::printStartupLogo(mcdk::getEnvIsPluginEnv());
        const auto config = mcdk::userParseConfig();
        mcdk::startGame(config);
#ifdef NDEBUG
    } catch (const std::exception& exception) {
        std::cerr << "[ERROR] " << exception.what() << '\n';
        return 1;
    }
#endif
    return 0;
}
