#include <mcdk/application.hpp>
#include <mcdk/config.hpp>
#include <mcdk/console_output.hpp>
#include <mcdk/env.hpp>
#include <mcdk/plugin_host/host.hpp>

#include <filesystem>

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

namespace {
// 插件终结的作用域守卫。
// 析构里绝不能让异常逃出去：它多半是在栈展开途中运行的，再抛一次就是
    struct PluginScope {
        ~PluginScope() {
            try {
                mcdk::plugin_host::instance().shutdown();
            } catch (const std::exception& exception) {
                std::cerr << "[ERROR] 插件终结失败: " << exception.what() << '\n';
            } catch (...) {
                std::cerr << "[ERROR] 插件终结失败\n";
            }
        }
    };

} // namespace

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

        // 插件只从 .mcdev.json 的 plugins 声明加载，宿主不扫描任何目录。
        // 相对路径以 .mcdev.json 所在目录（即当前工作目录）为基准。
        auto& pluginHost = mcdk::plugin_host::instance();
        pluginHost.loadDeclared(config.plugins, std::filesystem::current_path());
// 无论 startGame 怎么退出——正常结束、被插件否决、或中途抛异常——插件都必须
// 走完终结流程。正常路径上 launchGameExe 已经做过，Host::shutdown 幂等，
        const PluginScope pluginScope;

        pluginHost.advance(MCDK_STAGE_REGISTER);
        mcdk::startGame(config);
#ifdef NDEBUG
    } catch (const std::exception& exception) {
        std::cerr << "[ERROR] " << exception.what() << '\n';
        return 1;
    }
#endif
    return 0;
}
