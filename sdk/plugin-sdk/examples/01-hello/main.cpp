//
// 最小插件示例。
//
// 注意这里从头到尾没有出现任何 mcdk_ 前缀的 C 类型：std::string、
// std::string_view、字符串拼接随便用，转成 ABI 形态是 SDK 的事。
//
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

            // Dispatch::Main 的回调保证跑在宿主主线程上，适合要去碰
            // 非线程安全宿主状态的场景。代价是比 Queued 多一跳延迟。
            context.events().on<mcdk::ev::GameExit>(mcdk::Dispatch::Main, [&context](const auto& e) {
                context.console().info("event:game-exit-main:" + std::to_string(e.exitCode));
            });
        }

        void onConfig(mcdk::Context& context) override { context.console().print(mcdk::Color::Cyan, "stage:config"); }

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
