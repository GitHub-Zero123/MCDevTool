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

            // MCP 工具的注册窗口是 mcp.register.before 到注册表封存之间。
            // onRegister 太早——那时 mcdk 还没把工具注册表接进来。
            context.events().on<mcdk::ev::McpRegisterBefore>([&context](const auto&) {
                // 推荐写法：描述符写在 plugin.json 的 mcpTools 里，这里只补 handler。
                // 这样 mcdk 没跑的时候 mcdk_stdio_bridge 也能把它列进 tools/list。
                const auto bound =
                    context.mcp().bindTool("hello_echo", [](std::string_view args, std::string_view session) {
                        return std::string(R"({"echo":)") + std::string(args) + R"(,"session":")"
                             + std::string(session) + R"("})";
                    });
                context.console().info("mcp:bind-tool:" + std::to_string(bound));

                // 动态注册：描述符随调用传过去，不进清单。代价是启动前不可见。
                mcdk::ToolDesc dynamic;
                dynamic.name                 = "hello_dynamic";
                dynamic.description          = "运行期注册的工具";
                dynamic.inputSchema          = R"({"type":"object"})";
                dynamic.annotations.readOnly = true;
                const auto added =
                    context.mcp().addTool(dynamic, [](std::string_view args, std::string_view session) {
                        return std::string(R"({"echo":)") + std::string(args) + R"(,"session":")"
                             + std::string(session) + R"("})";
                    });
                context.console().info("mcp:add-tool:" + std::to_string(added));
            });
        }

        void onConfig(mcdk::Context& context) override { context.console().print(mcdk::Color::Cyan, "stage:config"); }

        void onWorld(mcdk::Context& context) override { context.console().print(mcdk::Color::Cyan, "stage:world"); }

        void onRuntime(mcdk::Context& context) override {
            context.console().print(mcdk::Color::Cyan, "stage:runtime");

            // 注册表此刻已封存，注册窗口已过，必须被拒。
            mcdk::ToolDesc late;
            late.name        = "hello_too_late";
            late.inputSchema = R"({"type":"object"})";
            const auto lateStatus =
                context.mcp().addTool(late, [](std::string_view, std::string_view) { return std::string("{}"); });
            context.console().info("mcp:late-tool:" + std::to_string(lateStatus));

            // mcdk.info：会话快照。ABI 那边这些字符串都是借用的，
            // SDK 已经拷成 std::string，这里随便用。
            const auto session = context.info().session();
            context.console().info(
                "info:" + std::to_string(session.mcpPort) + ":" + session.worldName + ":"
                + std::to_string(session.gamePid) + ":" + std::to_string(static_cast<int>(session.state))
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
