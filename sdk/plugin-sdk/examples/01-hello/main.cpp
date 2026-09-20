// 最小插件示例。
// 示例只使用 C++ 类型，SDK 负责转换到 ABI 形式。
#include <mcdk/plugin/plugin.hpp>

#include <string>

namespace {

    class HelloPlugin final : public mcdk::Plugin {
    public:
        void onRegister(mcdk::Context& context) override {
            // std::string 在这里完全自由；Console 收 string_view，内部转 mcdk_str。
            context.console().info("hello, host " + std::string(context.hostVersion()));

            // .mcdev.json 里这条声明的 config，原样 JSON 文本。
            // 同一个二进制声明多次、各带不同 config，就能有不同行为。
            context.console().info("config=" + std::string(context.configJson()));

            context.console().print(mcdk::Color::Cyan, "stage:register");

            // 事件订阅必须在 REGISTER 内完成：事件不重放，错过就收不到了。
            context.events().on<mcdk::ev::McpRegisterFinish>([&context](const auto& e) {
                context.console().info("event:mcp-register-finish:" + std::to_string(e.toolCount));
            });
            context.events().on<mcdk::ev::GameLaunchFinish>([&context](const auto& e) {
                context.console().info(
                    "event:game-launch-finish:" + std::to_string(e.pid) + ":" + std::string(e.exePath)
                );
            });

            // Sync 在发射线程上内联跑。ev::GameExit 由 mcdk 的启动线程发射，
            // 所以这个回调就在那条线程上。
            context.events().on<mcdk::ev::GameExit>(mcdk::Dispatch::Sync, [&context](const auto& e) {
                context.console().info("event:game-exit-sync:" + std::to_string(e.exitCode));
            });

            // MCP 工具只能在 REGISTER 阶段注册，SDK 负责转换其复杂参数。
            // handler 闭包都由 SDK 降级成 C 形态，这里一个 mcdk_ 类型都看不到。
            mcdk::ToolDesc tool;
            tool.name                 = "hello_echo";
            tool.description          = "回显参数";
            tool.inputSchema          = R"({"type":"object","properties":{"text":{"type":"string"}}})";
            tool.annotations.readOnly = true;
            const auto added = context.mcp().addTool(tool, [](std::string_view args, std::string_view session) {
                return std::string(R"({"echo":)") + std::string(args) + R"(,"session":")" + std::string(session)
                     + R"("})";
            });
            context.console().info("mcp:add-tool:" + std::to_string(added));
        }

        void onConfig(mcdk::Context& context) override {
            context.console().print(mcdk::Color::Cyan, "stage:config");
            // 注册窗口已过，必须被拒。
            mcdk::ToolDesc late;
            late.name        = "hello_too_late";
            late.inputSchema = R"({"type":"object"})";
            const auto status =
                context.mcp().addTool(late, [](std::string_view, std::string_view) { return std::string("{}"); });
            context.console().info("mcp:late-tool:" + std::to_string(status));
        }

        void onWorld(mcdk::Context& context) override { context.console().print(mcdk::Color::Cyan, "stage:world"); }

        void onRuntime(mcdk::Context& context) override {
            context.console().print(mcdk::Color::Cyan, "stage:runtime");

            // mcdk.info：会话快照。ABI 那边这些字符串都是借用的，
            // SDK 已经拷成 std::string，这里随便用。
            const auto session = context.info().session();
            context.console().info(
                "info:" + std::to_string(session.mcpPort) + ":" + session.worldName + ":"
                + std::to_string(session.gamePid)
            );

            // mcdk.log：游戏日志缓冲区。索引 0 是最新一条。
            const auto lines = context.log().latest(2);
            std::string rendered;
            for (const auto& entry : lines) {
                rendered += entry.text;
                rendered += "|";
            }
            context.console().info(
                "log:" + std::to_string(context.log().count()) + ":" + rendered
            );

            // mcdk.game：游戏没起来时必须是干净的失败，而不是崩溃或阻塞。
            // 这里用 try 系列拿状态码，不走异常。
            std::string pythonResult;
            const auto  status = context.game().tryExecutePython("1 + 1", pythonResult);
            context.console().info("game:exec-status:" + std::to_string(status));
            context.console().info("game:capture-empty:" + std::to_string(context.game().capture().empty() ? 1 : 0));
        }

        void onShutdown(mcdk::Context& context) override {
            context.console().print(mcdk::Color::DarkGray, "stage:shutdown");
        }
    };

} // namespace

MCDK_PLUGIN(HelloPlugin, "com.example.hello", "0.1.0");
