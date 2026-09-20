//
// 插件宿主的端到端验证：真的去加载 examples/01-hello 构建出的 DLL。
//
// 覆盖 M2 的验收路径：读声明 → 校验 → 调入口 → 推进各阶段 → 终结。
// 这不是 mock —— 走的是 LoadLibrary、GetProcAddress、真实的 C ABI 握手。
//
#include <mcdk/log_buffer.hpp>
#include <mcdk/plugin_host/events.hpp>
#include <mcdk/plugin_host/host.hpp>
#include <mcdk/plugin_host/session_binding.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>
#include <string>
#include <vector>

#ifndef MCDEV_TEST_HELLO_PLUGIN
#error "MCDEV_TEST_HELLO_PLUGIN must point at the built example plugin"
#endif

namespace {

    bool expect(bool condition, const char* description) {
        if (!condition) {
            std::cerr << "Failed: " << description << '\n';
        }
        return condition;
    }

    bool contains(const std::vector<std::string>& lines, std::string_view needle) {
        for (const auto& line : lines) {
            if (line.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

} // namespace

int main() {
    using namespace mcdk;

    const std::filesystem::path pluginPath = MCDEV_TEST_HELLO_PLUGIN;
    bool                        passed     = true;

    std::vector<std::string> output;
    plugin_host::Host        host([&output](const std::string& message, ConsoleColor) { output.push_back(message); });

    // --- 加载 --------------------------------------------------------
    std::vector<PluginDeclaration> declarations;
    declarations.push_back(
        PluginDeclaration{
            .enabled    = true,
            .path       = pluginPath.generic_string(),
            .id         = "com.example.hello",
            .configJson = R"({"mode":"test","level":3})",
            .priority   = 0,
        }
    );
    // 一条禁用的声明：必须完全不被加载。
    declarations.push_back(
        PluginDeclaration{
            .enabled = false,
            .path    = pluginPath.generic_string(),
        }
    );

    // 零插件契约：没加载任何插件时不存在主线程唤醒信号，游戏等待循环
    // 因此退回无期限阻塞，一次周期性唤醒都没有（12-performance.md §1）。
    passed &= expect(
        plugin_host::mainThreadWorkWaitHandle() == nullptr,
        "no main-thread wake signal exists before any plugin is loaded"
    );

    host.loadDeclared(declarations, pluginPath.parent_path());

#ifdef _WIN32
    passed &= expect(
        plugin_host::mainThreadWorkWaitHandle() != nullptr,
        "loading a plugin creates the main-thread wake signal"
    );
#endif

    const auto loaded  = host.loaded();
    passed            &= expect(loaded.size() == 1, "only the enabled declaration is loaded");
    if (loaded.size() == 1) {
        passed &= expect(loaded[0].id == "com.example.hello", "plugin id round-trips across the ABI");
        passed &= expect(loaded[0].version == "0.1.0", "plugin version round-trips across the ABI");
        passed &= expect(loaded[0].abiMajor == MCDK_ABI_VERSION_MAJOR, "plugin reports the host's ABI major");
        passed &= expect(!loaded[0].degraded, "a freshly loaded plugin is not degraded");
    }

    // --- 绑定一份假的运行期 -------------------------------------------
    // mcdk.info / mcdk.log 的数据源。真实运行时由 launchGameExe 绑定，
    // 这里自己造一份，因为测试不启动游戏。
    {
        auto logBuffer = std::make_shared<LogBuffer>();
        logBuffer->add("log-oldest");
        logBuffer->add("log-middle");
        logBuffer->add("log-newest");

        plugin_host::SessionBinding binding;
        binding.facts.mcpPort    = 1234;
        binding.facts.mcpEnabled = true;
        binding.facts.worldName  = "TestWorld";
        binding.logBuffer        = logBuffer;
        binding.errBuffer        = std::make_shared<LogBuffer>();
        plugin_host::bindSession(std::move(binding));
    }

    // --- 阶段推进 ----------------------------------------------------
    host.advance(MCDK_STAGE_REGISTER);
    host.advance(MCDK_STAGE_CONFIG);
    host.advance(MCDK_STAGE_WORLD);
    host.advance(MCDK_STAGE_RUNTIME);
    host.advance(MCDK_STAGE_SHUTDOWN);

    passed &= expect(contains(output, "stage:register"), "onRegister ran");
    passed &= expect(contains(output, "stage:config"), "onConfig ran");
    passed &= expect(contains(output, "stage:world"), "onWorld ran");
    passed &= expect(contains(output, "stage:runtime"), "onRuntime ran");
    passed &= expect(contains(output, "stage:shutdown"), "onShutdown ran");

    // 插件打的日志必须带上它的 id，用户才分得清是谁打的。
    passed &= expect(contains(output, "[com.example.hello]"), "plugin output is tagged with its id");

    // --- mcdk.info / mcdk.log ---------------------------------------
    // 插件在 onRuntime 里读了两张表，输出应当反映上面绑进去的数据。
    passed &= expect(contains(output, "info:1234:TestWorld:0"), "mcdk.info 的会话快照跨过了 ABI");
    // latest(2) 默认最新在前，共 3 条。
    passed &= expect(
        contains(output, "log:3:log-newest|log-middle|"),
        "mcdk.log 的回调式枚举保持了「索引 0 = 最新」的语义"
    );

    // --- mcdk.game 的降级行为 ------------------------------------
    // 本测试不启动游戏，所以两个能力都应当干净地失败。这比「能跑」更重要：
    // 插件在游戏起来之前调这两个接口是必然会发生的。
    passed &= expect(
        contains(output, "game:exec-status:" + std::to_string(MCDK_ERR_GAME_NOT_READY)),
        "execute_python 在游戏未就绪时返回 GAME_NOT_READY 而非阻塞或崩溃"
    );
    passed &= expect(contains(output, "game:capture-empty:1"), "capture 在游戏未就绪时返回空图");

    // --- 握手：宿主版本与 config 都跨越了 C ABI 并被 SDK 还原成 std::string ---
    passed &= expect(contains(output, "hello, host 0.1.0"), "host version crosses the ABI");
    passed &= expect(
        contains(output, R"(config={"mode":"test","level":3})"),
        "the declaration's config JSON reaches the plugin verbatim"
    );

    // --- 事件 --------------------------------------------------------
    // 01-hello 在 onRegister 里订阅了两个事件（默认 QUEUED）。这里手动发射，
    // 验证订阅 → 深拷贝入队 → 派发线程回调 → 插件侧 SDK 还原成具名字段整条链。
    {
        using namespace mcdk::plugin_host;
        MCDK_EMIT(EventId::McpRegisterFinish, [] {
            mcdk_ev_mcp_register payload{};
            payload.struct_size = static_cast<std::uint32_t>(sizeof(payload));
            payload.tool_count  = 9;
            return payload;
        });
        {
            // 字符串字段的存储故意在发射后立即析构：
            // QUEUED 的派发线程晚得多，总线必须真的拷贝一份字节。
            std::string exePathUtf8 = "D:/games/Minecraft.exe";
            MCDK_EMIT(EventId::GameLaunchFinish, [&] {
                mcdk_ev_game_launch_finish payload{};
                payload.struct_size  = static_cast<std::uint32_t>(sizeof(payload));
                payload.pid          = 4242;
                payload.exe_path.ptr = exePathUtf8.data();
                payload.exe_path.len = exePathUtf8.size();
                return payload;
            });
            // 立即原地抹掉：总线若只 memcpy 了 mcdk_str 里的指针，
            // 派发线程读到的就是这一行写进去的字节。
            std::fill(exePathUtf8.begin(), exePathUtf8.end(), 'X');
        }
        // QUEUED 是异步的，等派发线程把队列吃完。
        for (int attempt = 0; attempt < 200 && !contains(output, "event:game-launch-finish:4242:"); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        passed &= expect(contains(output, "event:mcp-register-finish:9"), "a queued event reaches the plugin");
        passed &= expect(
            contains(output, "event:game-launch-finish:4242:D:/games/Minecraft.exe"),
            "the typed payload fields, strings included, survive the round trip through the C ABI"
        );
    }

    // --- MAIN 派发 --------------------------------------------------
    // Dispatch::Main 的回调只能在 pumpMainThreadWork() 里跑。先确认它不会
    // 自己跑到派发线程上去（那样 Main 就退化成了 Queued）。
    {
        using namespace mcdk::plugin_host;
        MCDK_EMIT(EventId::GameExit, [] {
            mcdk_ev_game_exit payload{};
            payload.struct_size = static_cast<std::uint32_t>(sizeof(payload));
            payload.pid         = 4242;
            payload.exit_code   = 7;
            return payload;
        });
        // 足够让派发线程把它搬到主线程队列里。
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        passed &= expect(
            !contains(output, "event:game-exit-main:7"),
            "a Dispatch::Main handler does not run until the main thread pumps"
        );
        pumpMainThreadWork();
        passed &= expect(
            contains(output, "event:game-exit-main:7"),
            "a Dispatch::Main handler runs on the pumping thread"
        );
    }

    // 没有订阅者的事件必须走零开销路径，且不能崩。
    {
        using namespace mcdk::plugin_host;
        passed &= expect(!hasSubscribers(EventId::LogError), "nobody subscribed to log.error");
        MCDK_EMIT(EventId::LogError, [] {
            mcdk_ev_log_line payload{};
            payload.struct_size = static_cast<std::uint32_t>(sizeof(payload));
            return payload;
        });
    }

    // --- 终结 --------------------------------------------------------
    host.shutdown();
    plugin_host::unbindSession();
    passed &= expect(host.empty(), "shutdown retires every plugin");
    // 二次 shutdown 必须是空操作而不是崩溃。
    host.shutdown();
    passed &= expect(host.empty(), "shutdown is idempotent");

    if (!passed) {
        std::cerr << "--- captured output ---\n";
        for (const auto& line : output) {
            std::cerr << line << '\n';
        }
        std::cerr << "plugin_host_test failed\n";
        return 1;
    }
    std::cout << "plugin_host_test passed\n";
    return 0;
}
