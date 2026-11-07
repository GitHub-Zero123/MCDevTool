#include <mcdk/application.hpp>
#include <mcdk/cli.hpp>
#include <mcdk/config.hpp>
#include <mcdk/console_output.hpp>
#include <mcdk/env.hpp>

#include <cstdio>
#include <exception>
#include <iostream>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
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

    // 带参数走自动化 CLI，不带参数是编辑器和手动使用的交互式启动。
    if (argc > 1) {
        return mcdk::cli::parse(argc, argv);
    }

#ifdef NDEBUG
    try {
#endif
        mcdk::printStartupLogo(mcdk::getEnvIsPluginEnv());
        mcdk::runGame(mcdk::userParseConfig());
#ifdef NDEBUG
    } catch (const std::exception& exception) {
        std::cerr << "[ERROR] " << exception.what() << '\n';
        return 1;
    }
#endif
    return 0;
}
