// 自定义游戏启动器示例：接管进程创建，追加命令行参数与环境变量后自己起进程。
// 注入 DLL、套调试器等策略也是同一个骨架：在 createSuspended 与 commitProcess 之间动手。
#include <mcdk/plugin/plugin.hpp>
#include <mcdk/plugin/process.hpp>

#include <string>
#include <string_view>

namespace {

    // 朴素取值，避免给 SDK 示例引入 JSON 依赖。
    [[nodiscard]] std::string field(std::string_view json, std::string_view key) {
        const std::string needle = "\"" + std::string(key) + "\":\"";
        const auto        start  = json.find(needle);
        if (start == std::string_view::npos) {
            return {};
        }
        const auto valueBegin = start + needle.size();
        const auto valueEnd   = json.find('"', valueBegin);
        return valueEnd == std::string_view::npos ? std::string{}
                                                  : std::string(json.substr(valueBegin, valueEnd - valueBegin));
    }

    class LauncherPlugin final : public mcdk::Plugin {
    public:
        void onRegister(mcdk::Context& context) override {
            const auto config = context.configJson();
            mExtraArgs        = field(config, "extra_args");
            mEnvName          = field(config, "env_name");
            mEnvValue         = field(config, "env_value");

            context.events().on<mcdk::ev::GameProcessCreate>([this, &context](const auto& e) {
                auto spec = mcdk::process::LaunchSpec::from(e);
                if (!mExtraArgs.empty()) {
                    spec.commandLine += L' ';
                    spec.commandLine += mcdk::process::widen(mExtraArgs);
                }
                if (!mEnvName.empty()) {
                    spec.setEnvironment(mcdk::process::widen(mEnvName), mcdk::process::widen(mEnvValue));
                }
                const auto result = mcdk::process::launch(context.game(), e, std::move(spec));
                context.console().info(result == mcdk::EventResult::Stop ? "launcher:committed" : "launcher:failed");
                return result;
            });
        }

    private:
        std::string mExtraArgs;
        std::string mEnvName;
        std::string mEnvValue;
    };

} // namespace

MCDK_PLUGIN(LauncherPlugin, "com.example.launcher", "0.1.0");
