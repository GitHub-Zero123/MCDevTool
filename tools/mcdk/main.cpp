// MCDK 启动入口（薄壳）
//
// 启动逻辑已迁移至 mcdk_core 库（src/core.cpp），本文件仅负责：
//   - 控制台初始化（UTF-8 输出、伪终端 setvbuf）
//   - CLI 子命令拦截（MCDK_ENABLE_CLI 时）
//   - 读取 .mcdev.json 并调用 mcdk::core::Launcher::run
//
// 行为与重构前完全一致。By Zero123
#include <filesystem>
#include <iostream>

#include <mcdk_core/core.h>
#include "modules/env.hpp" // mcdk::getEnvOutputMode

#ifdef _WIN32
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
        mcdk::core::LaunchOptions options;
        options.printLogo = true;
        // 沿用原 userParseConfig 行为：读取当前目录 .mcdev.json，不存在则自动生成默认配置
        options.config = mcdk::core::loadConfigFromFile(std::filesystem::current_path() / ".mcdev.json");
        return mcdk::core::Launcher::run(options);
#ifdef NDEBUG
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << "\n";
        return 1;
    }
#endif
    return 0;
}
