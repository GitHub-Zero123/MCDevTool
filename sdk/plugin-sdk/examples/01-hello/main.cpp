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
        }

        void onConfig(mcdk::Context& context) override { context.console().print(mcdk::Color::Cyan, "stage:config"); }

        void onWorld(mcdk::Context& context) override { context.console().print(mcdk::Color::Cyan, "stage:world"); }

        void onRuntime(mcdk::Context& context) override { context.console().print(mcdk::Color::Cyan, "stage:runtime"); }

        void onShutdown(mcdk::Context& context) override {
            context.console().print(mcdk::Color::DarkGray, "stage:shutdown");
        }
    };

} // namespace

MCDK_PLUGIN(HelloPlugin, "com.example.hello", "0.1.0");
