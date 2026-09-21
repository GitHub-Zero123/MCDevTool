// 插件宿主的端到端验证：真的去加载 examples/01-hello 构建出的 DLL。
// 覆盖 M2 的验收路径：读声明 → 校验 → 调入口 → 推进各阶段 → 终结。
#include <mcdk/log_buffer.hpp>
#include <mcdk/version.hpp>
#include <mcdk/runtime/mcp_tool_registry.hpp>
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

    host.loadDeclared(declarations, pluginPath.parent_path());


    const auto loaded  = host.loaded();
    passed            &= expect(loaded.size() == 1, "only the enabled declaration is loaded");
    if (loaded.size() == 1) {
        passed &= expect(loaded[0].id == "com.example.hello", "plugin id round-trips across the ABI");
        passed &= expect(loaded[0].version == "0.1.0", "plugin version round-trips across the ABI");
        passed &= expect(loaded[0].abiMajor == MCDK_ABI_VERSION_MAJOR, "plugin reports the host's ABI major");
        passed &= expect(!loaded[0].degraded, "a freshly loaded plugin is not degraded");
    }

    auto toolRegistry = std::make_shared<runtime::McpToolRegistry>();
// --- 绑定一份假的运行期 -------------------------------------------
// mcdk.info / mcdk.log 的数据源。真实运行时由 launchGameExe 绑定，
    {
        auto logBuffer = std::make_shared<LogBuffer>();
        logBuffer->add("log-oldest");
        logBuffer->add("log-middle");
        logBuffer->add("log-newest");

        plugin_host::SessionBinding binding;
        binding.mcpToolRegistry  = toolRegistry;
        binding.facts.mcpPort    = 1234;
        binding.facts.mcpEnabled = true;
        binding.facts.worldName  = "TestWorld";
        binding.logBuffer        = logBuffer;
        binding.errBuffer        = std::make_shared<LogBuffer>();
        plugin_host::bindSession(std::move(binding));
    }

    // --- 阶段推进 ----------------------------------------------------
    // 顺序照搬 launchGameExe：WORLD 之后才开注册窗口，封存之后才进 RUNTIME。
    host.advance(MCDK_STAGE_REGISTER);
    host.advance(MCDK_STAGE_CONFIG);
    host.advance(MCDK_STAGE_WORLD);

    host.declareMcpTools(*toolRegistry);
    MCDK_EMIT(plugin_host::EventId::McpRegisterBefore, [] {
        mcdk_ev_mcp_register payload{};
        payload.struct_size = static_cast<std::uint32_t>(sizeof(payload));
        payload.tool_count  = 0;
        return payload;
    });
    toolRegistry->seal();

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
    // 未绑定追踪器时状态是 Unavailable(0)。
    passed &= expect(contains(output, "info:1234:TestWorld:0:0"), "mcdk.info 的会话快照跨过了 ABI");
    // latest(2) 默认最新在前，共 3 条。
    passed &= expect(
        contains(output, "log:3:log-newest|log-middle|"),
        "mcdk.log 的回调式枚举保持了「索引 0 = 最新」的语义"
    );

    // --- mcdk.mcp ----------------------------------------------------
    passed &= expect(contains(output, "mcp:add-tool:0"), "注册窗口内的 add_tool 成功");
    // 本夹具直指动态库、没有清单，所以 hello_echo 没被声明过。
    passed &= expect(
        contains(output, "mcp:bind-tool:" + std::to_string(MCDK_ERR_NOT_FOUND)),
        "bind_tool 绑一个清单里没声明的名字时失败，而不是凭空造一个工具"
    );
    passed &= expect(
        contains(output, "mcp:late-tool:" + std::to_string(MCDK_ERR_WRONG_STAGE)),
        "注册表封存之后 add_tool 返回 WRONG_STAGE 而不是默默成功"
    );

    const auto* entry = toolRegistry->find("hello_dynamic");
    passed           &= expect(entry != nullptr, "插件的工具进了共享注册表");
    if (entry != nullptr) {
        passed &= expect(entry->owner == "com.example.hello", "工具记下了来源插件 id");
        passed &= expect(entry->descriptor.description == "运行期注册的工具", "description 跨过了 ABI");
        // std::optional<bool> 被拆成两个位掩码又装回来，这里盯的就是那一跑。
        passed &= expect(
            entry->descriptor.annotations.read_only_hint.value_or(false),
            "readOnly 注解经位掩码往返后仍是 true"
        );
        passed &= expect(
            !entry->descriptor.annotations.destructive_hint.has_value(),
            "没设过的注解仍然是「没设」，而不是 false"
        );

        // 真的调一次 handler，走完整的 JSON 文本 → C ABI → 插件 → TLS → 解析回程。
        const auto response = entry->handler(mcp::json{{"text", "hi"}}, "session-42");
        passed             &= expect(
            response.contains("echo") && response["echo"]["text"] == "hi",
            "工具参数往返都经过了 JSON 文本形态"
        );
        passed &= expect(response.value("session", "") == "session-42", "session id 传给了 handler");
    }
// --- mcdk.game 的降级行为 ------------------------------------
// 本测试不启动游戏，因此两个能力都应明确返回未就绪。
    passed &= expect(
        contains(output, "game:exec-status:" + std::to_string(MCDK_ERR_GAME_NOT_READY)),
        "execute_python 在游戏未就绪时返回 GAME_NOT_READY 而非阻塞或崩溃"
    );
    passed &= expect(contains(output, "game:capture-empty:1"), "capture 在游戏未就绪时返回空图");

    // --- 握手：宿主版本与 config 都跨越了 C ABI 并被 SDK 还原成 std::string ---
    passed &= expect(
        contains(output, "hello, host " + std::string(mcdk::kVersion)),
        "host version crosses the ABI"
    );
    passed &= expect(
        contains(output, R"(config={"mode":"test","level":3})"),
        "the declaration's config JSON reaches the plugin verbatim"
    );
// --- 事件 --------------------------------------------------------
// 01-hello 在 onRegister 里订阅事件；这里手动发射验证异步派发。
    {
        using namespace mcdk::plugin_host;
        MCDK_EMIT(EventId::McpRegisterFinish, [] {
            mcdk_ev_mcp_register payload{};
            payload.struct_size = static_cast<std::uint32_t>(sizeof(payload));
            payload.tool_count  = 9;
            return payload;
        });
        {
            // 发射后立即析构字符串，验证 QUEUED 派发会深拷贝 payload。
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
