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
            const std::string greeting = "hello from 01-hello, host " + std::string(context.hostVersion());
            context.console().info(greeting);
            context.console().print(mcdk::Color::Cyan, "注册阶段：可以在此挂事件与 MCP 工具");
        }

        void onRuntime(mcdk::Context& context) override { context.console().info("运行阶段：游戏已启动"); }

        void onShutdown(mcdk::Context& context) override {
            context.console().print(mcdk::Color::DarkGray, "01-hello 退出");
        }
    };

} // namespace

MCDK_PLUGIN(HelloPlugin, "com.example.hello", "0.1.0");
