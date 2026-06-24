// mcdk_core 冒烟测试
//
// 验证目标：
//   1. mcdk_core 库可被独立链接、API 可调用。
//   2. Launcher::run 接受强类型 LaunchConfig 入参，不依赖磁盘 .mcdev.json 文件。
//   3. 在缺少有效 game_executable_path 时，应通过抛异常而非崩溃退出。
//   4. LaunchConfig::fromJson / toJson 适配层对"全字段配置"可逐字段往返（round-trip）。
//
// 注意：本测试不真正启动游戏进程（会卡在子进程等待），仅校验 API 入口、配置流转与互转适配层。
// By Zero123
#include <array>
#include <cstdint>
#include <iostream>
#include <filesystem>
#include <mcdk_core/core.h>

namespace {

    // 构造一个"全字段都非默认"的嵌套强类型配置，用于 round-trip 校验。
    mcdk::core::LaunchConfig makeFullConfig() {
        mcdk::core::LaunchConfig c;
        c.gameExecutablePath = "/nonexistent/path/minecraft.exe";
        c.autoJoinGame       = false;

        // world
        c.world.seed            = 123456789ULL;
        c.world.reset           = true;
        c.world.name            = "RT_WORLD";
        c.world.folderName      = "RT_FOLDER";
        c.world.type            = 2;
        c.world.gameMode        = 0;
        c.world.enableCheats    = false;
        c.world.keepInventory   = false;
        c.world.doWeatherCycle  = false;
        c.world.doDaylightCycle = false;
        c.world.experiments     = mcdk::core::LaunchConfig::Experiments{true, true, true};

        // player
        c.player.name = "tester";
        c.player.skin = mcdk::core::LaunchConfig::SkinInfo{true, "/skins/custom.png"};

        // mod 目录
        c.includedModDirs = {
            {"./", true, true},             // 全默认 → toJson 回写为字符串
            {"mods/extra", false, true},    // 对象形式（hot_reload 覆写）
            {"mods/disabled", true, false}, // 对象形式（enabled=false，fromJson 不过滤）
        };

        // hotReload
        c.hotReload.mods      = false;
        c.hotReload.ui        = true;
        c.hotReload.shaders   = true;
        c.hotReload.materials = true;

        // debug
        c.debug.includeDebugMod = false;
        c.debug.ptvsd           = {true, "0.0.0.0", 5005};
        c.debug.modpc           = {true, 6000};
        c.debug.options         = nlohmann::json{{"verbose", true}, {"level", 3}};

        // 服务 / 外观
        c.mcpServer            = {true, "127.0.0.1", 20000};
        c.netease              = {true};
        c.windowStyle.alwaysOnTop   = true;
        c.windowStyle.hideTitleBar  = true;
        c.windowStyle.titleBarColor = std::array<std::uint8_t, 3>{10, 20, 30};
        c.windowStyle.fixedSize     = std::array<int, 2>{1280, 720};
        c.windowStyle.fixedPosition = std::array<int, 2>{100, 50};
        c.windowStyle.lockCorner    = 2;
        return c;
    }

} // namespace

int main() {
    int failures = 0;

    // ---------- 用例1：默认（空 game_executable_path）config 应抛异常 ----------
    // 默认 LaunchConfig 的 gameExecutablePath 为空，run() 内部 startGame 应因无法定位有效游戏 exe
    // （自动搜索失败抛异常，或搜索成功但 prompter 为 nullptr 抛异常）而终止，绝不正常返回。
    try {
        mcdk::core::LaunchOptions options;
        options.printLogo = false;
        mcdk::core::Launcher::run(options);
        std::cerr << "[FAIL] 用例1: 期望抛异常，但 run() 正常返回。\n";
        ++failures;
    } catch (const std::exception& e) {
        std::cout << "[PASS] 用例1: 空 config 正确抛异常: " << e.what() << "\n";
    }

    // ---------- 用例2：强类型 config 带字段也能流转 ----------
    // 构造一个含字段的嵌套强类型 config（仍然无效 exe 路径），验证字段被读取而非依赖磁盘文件。
    try {
        mcdk::core::LaunchOptions options;
        options.config.gameExecutablePath     = "/nonexistent/path/minecraft.exe";
        options.config.world.folderName       = "SMOKE_TEST_WORLD";
        options.config.autoJoinGame           = false;
        options.config.debug.includeDebugMod  = false;
        options.printLogo                     = false;
        mcdk::core::Launcher::run(options);
        std::cerr << "[FAIL] 用例2: 无效 exe 路径应抛异常。\n";
        ++failures;
    } catch (const std::exception& e) {
        std::cout << "[PASS] 用例2: 强类型 config 字段流转，抛异常: " << e.what() << "\n";
    }

    // ---------- 用例3：自定义 logger 回调可注入 ----------
    {
        bool                      loggerCalled = false;
        mcdk::core::LaunchOptions options;
        options.printLogo = true; // 触发 logo 输出以验证 logger 注入
        options.logger    = [&loggerCalled](const std::string& msg, mcdk::ConsoleColor) {
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

    // ---------- 用例4：fromJson/toJson 全字段往返 ----------
    {
        auto full = makeFullConfig();
        auto j1   = full.toJson();
        auto rt   = mcdk::core::LaunchConfig::fromJson(j1);
        auto j2   = rt.toJson();

        if (j1 == j2) {
            std::cout << "[PASS] 用例4a: toJson → fromJson → toJson 幂等往返一致。\n";
        } else {
            std::cerr << "[FAIL] 用例4a: round-trip 不一致。\n  j1=" << j1.dump() << "\n  j2=" << j2.dump() << "\n";
            ++failures;
        }

        // 逐块抽查若干关键字段确实被适配层保留
        bool fieldsOk = rt.world.seed.has_value() && rt.world.seed.value() == 123456789ULL
                        && rt.world.type == 2 && rt.world.gameMode == 0 && rt.world.enableCheats == false
                        && rt.world.experiments.has_value() && rt.world.experiments->dataDrivenBiomes
                        && rt.player.name == "tester"
                        && rt.player.skin.has_value() && rt.player.skin->slim && rt.player.skin->skin == "/skins/custom.png"
                        && rt.hotReload.ui && !rt.hotReload.mods
                        && !rt.debug.includeDebugMod
                        && rt.debug.ptvsd.enabled && rt.debug.ptvsd.port == 5005
                        && rt.debug.modpc.enabled && rt.debug.modpc.port == 6000
                        && rt.debug.options.is_object() && rt.debug.options.value("level", 0) == 3
                        && rt.mcpServer.enabled && rt.mcpServer.serverPort == 20000
                        && rt.netease.chatExtension
                        && rt.windowStyle.alwaysOnTop && rt.windowStyle.titleBarColor.has_value()
                        && (*rt.windowStyle.titleBarColor)[1] == 20
                        && rt.windowStyle.lockCorner.has_value() && rt.windowStyle.lockCorner.value() == 2
                        && rt.includedModDirs.size() == 3 && rt.includedModDirs[2].enabled == false;
        if (fieldsOk) {
            std::cout << "[PASS] 用例4b: 关键字段逐块往返保留正确。\n";
        } else {
            std::cerr << "[FAIL] 用例4b: 关键字段往返丢失或不一致。\n";
            ++failures;
        }

        // 缺省（空对象）→ fromJson → 全默认值校验：核心模式直接构造与 fromJson({}) 应等价
        auto empty = mcdk::core::LaunchConfig::fromJson(nlohmann::json::object());
        bool defaultsOk = empty.world.folderName == "MC_DEV_WORLD" && empty.world.name == "MC_DEV_WORLD"
                          && empty.autoJoinGame && empty.debug.includeDebugMod && empty.world.type == 1
                          && empty.world.gameMode == 1 && empty.world.enableCheats && empty.world.keepInventory
                          && empty.world.doWeatherCycle && empty.world.doDaylightCycle && empty.player.name == "developer"
                          && !empty.world.seed.has_value() && !empty.world.experiments.has_value()
                          && !empty.player.skin.has_value() && empty.debug.options.is_null()
                          && empty.mcpServer.serverPort == 19133 && empty.debug.ptvsd.port == 56788
                          && empty.debug.modpc.port == 5632 && !empty.netease.chatExtension
                          && empty.hotReload.mods && !empty.hotReload.ui
                          && empty.includedModDirs.size() == 1 && empty.includedModDirs[0].path == "./";
        if (defaultsOk) {
            std::cout << "[PASS] 用例4c: fromJson({}) 默认值与原 .value(key,default) 一致。\n";
        } else {
            std::cerr << "[FAIL] 用例4c: fromJson({}) 默认值不符。\n";
            ++failures;
        }
    }

    // ---------- 用例5：included_mod_dirs 非法元素 → 跳过 + 结构化诊断（不抛异常）----------
    {
        using Diag = mcdk::core::LaunchConfig::Diagnostic;
        nlohmann::json bad = {
            {"included_mod_dirs", nlohmann::json::array({"./", 123, nlohmann::json{{"path", "mods/x"}}})}
        };
        std::vector<Diag> diags;
        auto cfg = mcdk::core::LaunchConfig::fromJson(bad, &diags);
        // 合法的 "./" 与对象元素保留，非法的 123 被跳过；诊断恰好 1 条，且字段机器可判定（无需解析文案）
        bool ok = cfg.includedModDirs.size() == 2 && cfg.includedModDirs[0].path == "./"
                  && cfg.includedModDirs[1].path == "mods/x" && diags.size() == 1
                  && diags[0].code == Diag::Code::InvalidModDirElement
                  && diags[0].pointer == "/included_mod_dirs/1";
        if (ok) {
            std::cout << "[PASS] 用例5: 非法 mod 目录元素被跳过 + 结构化诊断（未抛异常）: " << diags[0].message() << "\n";
        } else {
            std::cerr << "[FAIL] 用例5: 非法元素处理或诊断不符（dirs=" << cfg.includedModDirs.size()
                      << ", diags=" << diags.size() << "）。\n";
            ++failures;
        }
    }

    if (failures == 0) {
        std::cout << "\n所有用例通过\n";
        return 0;
    }
    std::cerr << "\n失败用例: " << failures << "\n";
    return 1;
}
