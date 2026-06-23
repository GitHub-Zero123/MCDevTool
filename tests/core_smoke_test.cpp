// mcdk_core 冒烟测试
//
// 验证目标：
//   1. mcdk_core 库可被独立链接、API 可调用。
//   2. Launcher::run 接受纯内存 config 入参，不依赖磁盘 .mcdev.json 文件。
//   3. 在缺少有效 game_executable_path 时，应通过抛异常而非崩溃退出。
//
// 注意：本测试不真正启动游戏进程（会卡在子进程等待），仅校验 API 入口与配置流转。
// By Zero123
#include <iostream>
#include <filesystem>
#include <mcdk_core/core.h>

int main() {
    int failures = 0;

    // ---------- 用例1：纯内存空 config 应抛异常 ----------
    // 传入空 config（game_executable_path 缺失），
    // run() 内部 startGame 应因无法定位游戏 exe 抛 std::runtime_error。
    try {
        mcdk::core::LaunchOptions options;
        options.config = nlohmann::json::object(); // 纯内存，不读任何文件
        options.printLogo = false;
        mcdk::core::Launcher::run(options);
        std::cerr << "[FAIL] 用例1: 期望抛异常，但 run() 正常返回。\n";
        ++failures;
    } catch (const std::exception& e) {
        std::cout << "[PASS] 用例1: 空 config 正确抛异常: " << e.what() << "\n";
    }

    // ---------- 用例2：纯内存 config 带字段也能流转 ----------
    // 构造一个含字段的内存 config（仍然无效 exe 路径），验证字段被读取而非依赖磁盘文件。
    try {
        mcdk::core::LaunchOptions options;
        options.config = nlohmann::json{
            {"game_executable_path", "/nonexistent/path/minecraft.exe"},
            {"world_folder_name", "SMOKE_TEST_WORLD"},
            {"auto_join_game", false},
            {"include_debug_mod", false}
        };
        options.printLogo = false;
        mcdk::core::Launcher::run(options);
        std::cerr << "[FAIL] 用例2: 无效 exe 路径应抛异常。\n";
        ++failures;
    } catch (const std::exception& e) {
        std::cout << "[PASS] 用例2: 内存 config 字段流转，抛异常: " << e.what() << "\n";
    }

    // ---------- 用例3：自定义 logger 回调可注入 ----------
    {
        bool loggerCalled = false;
        mcdk::core::LaunchOptions options;
        options.config = nlohmann::json::object();
        options.printLogo = true; // 触发 logo 输出以验证 logger 注入
        options.logger = [&loggerCalled](const std::string& msg, mcdk::ConsoleColor) {
            loggerCalled = true;
            (void)msg;
        };
        try {
            mcdk::core::Launcher::run(options);
        } catch (const std::exception&) {
            // 预期抛异常（空 config）
        }
        if (loggerCalled) {
            std::cout << "[PASS] 用例3: 自定义 logger 回调被调用。\n";
        } else {
            std::cerr << "[FAIL] 用例3: 自定义 logger 未被调用。\n";
            ++failures;
        }
    }

    if (failures == 0) {
        std::cout << "\n所有用例通过 (3/3)\n";
        return 0;
    }
    std::cerr << "\n失败用例: " << failures << "\n";
    return 1;
}
