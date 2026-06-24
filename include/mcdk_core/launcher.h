// mcdk_core: Launcher API
//
// 对外启动入口。把原先写死在 tools/mcdk/main.cpp 里的启动逻辑封装成可复用 API，
// 允许其他业务以"几行代码 + 纯内存 config"的方式启动开发测试世界，而不必依赖磁盘上的 .mcdev.json。
//
// 设计要点：
//   - 行为与原 mcdk.exe 完全一致（logo 输出、世界生成、addon 注册、IPC/MCP/热更新/样式处理器编排）。
//   - LaunchOptions.config 为纯内存 JSON 入参，由调用方自行构造（可借助 createDefaultConfig() 或 loadConfigFromFile()）。
//   - logger 回调默认为 nullptr，使用内置的 Win32 彩色输出（等价于原 printColoredAtomic）。
//
// By Zero123
#pragma once

#include <string>
#include <functional>
#include <filesystem>
#include <nlohmann/json.hpp>
#include "launch_config.h" // 强类型 LaunchConfig
#include "console.hpp" // ConsoleColor / ConsoleOutputCallback（由 mcdk_core 的 modules include 目录提供）

namespace mcdk::core {

    // .mcdev.json 配置文件的读写策略（解决"库 API 背着业务方改磁盘文件"的问题）。
    enum class ConfigFilePolicy {
        ReadOnly,     // 不读不写 .mcdev.json（纯内存，默认）——核心模式主路径
        ReadOrCreate, // 读，缺失则创建默认（原 createDefaultConfig 写回行为）
        ReadWrite,    // 读 + 允许回写（如游戏路径修复，原 CLI 行为）
    };

    // 交互回调：遇到需要用户确认的场景（如游戏路径无效是否回写）时调用。
    // 返回 true 表示"同意/是"。nullptr 时核心模式直接抛异常（不弹交互）。
    using PromptCallback = std::function<bool(const std::string& question)>;

    // 启动选项
    struct LaunchOptions {
        // 强类型配置。调用方可直接构造（核心模式），或用 LaunchConfig::fromJson(loadConfigFromFile(...)) 适配（CLI）。
        LaunchConfig config;
        // 日志输出回调；nullptr 时使用内置 Win32 彩色输出（等价于原 mcdk.exe 行为）。
        ConsoleOutputCallback logger = nullptr;
        // 是否在启动前打印 logo（等价于原 printStartupLogo）。默认 false（由 main 决定是否打印）。
        bool printLogo = false;
        // 是否为插件环境（影响 logo 文案与 level.dat 更新策略）。默认从环境变量 MCDEV_IS_PLUGIN_ENV 读取。
        // 设为 -1 表示沿用环境变量；0/1 强制覆盖。
        int pluginEnv = -1;
        // 配置文件读写策略。默认 ReadOnly（核心模式不碰磁盘 .mcdev.json）。
        ConfigFilePolicy configFilePolicy = ConfigFilePolicy::ReadOnly;
        // 交互回调。默认 nullptr（需要交互时抛异常）；CLI 注入基于 std::cin 的实现以保持原行为。
        PromptCallback prompter = nullptr;
    };

    // 启动器：封装完整的"世界生成 + 进程启动 + 服务编排"流程。
    class Launcher {
    public:
        // 一键启动（最简，等价于原 mcdk.exe 无参运行的完整流程）。
        // 返回进程退出码：0 成功，1 异常。
        static int run(LaunchOptions options);
    };

    // 从磁盘加载 .mcdev.json 配置；文件不存在时自动创建默认配置并写回（等价于原 userParseConfig 行为）。
    nlohmann::json loadConfigFromFile(const std::filesystem::path& configPath);

    // 创建默认配置（等价于原 createDefaultConfig，会尝试自动匹配游戏 exe 路径）。
    nlohmann::json createDefaultConfig();

    // 打印启动 logo（等价于原 printStartupLogo）。pluginEnv 为 true 时显示插件环境文案。
    void printLogo(bool pluginEnv);

} // namespace mcdk::core
