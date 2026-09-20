//
// 加载器型插件：一个 DLL 充当其他插件的宿主。
//
// 这是 config 字段真正的用途。设想一个 Python / Lua 绑定宿主：DLL 只有一份，
// 但在 .mcdev.json 里被声明多次，每条 config 指向不同的脚本：
//
//   "plugins": [
//     { "enable": true, "path": "./plugins/py-host/py_host.dll",
//       "id": "com.me.py.hud",
//       "config": { "id": "com.me.py.hud",  "name": "HUD",   "script": "hud/main.py" } },
//     { "enable": true, "path": "./plugins/py-host/py_host.dll",
//       "id": "com.me.py.timer",
//       "config": { "id": "com.me.py.timer","name": "Timer", "script": "timer/main.py" } }
//   ]
//
// 两条声明 = 两个独立实例，各自持有自己的 Context、config 与解释器状态。
//
// 关键在于 identity()：五个脚本若都报出宿主 DLL 那个编译期写死的 id，它们在
// mcdk 眼里就是同一个插件——日志分不清谁是谁，声明里的 id 防替换校验失效，
// 将来的重名检测和依赖解析也一并失效。所以加载器必须按 config 报出动态身份。
//
// 本示例不真的嵌入解释器，只把「脚本」当成一个名字走完整个流程。
//
#include <mcdk/plugin/plugin.hpp>

#include <string>
#include <string_view>

namespace {

    // 朴素取值，避免给示例引入 JSON 依赖。真实的加载器应当用正经解析器。
    [[nodiscard]] std::string field(std::string_view json, std::string_view key) {
        const std::string needle = "\"" + std::string(key) + "\":\"";
        const auto        start  = json.find(needle);
        if (start == std::string_view::npos) {
            return {};
        }
        const auto valueBegin = start + needle.size();
        const auto valueEnd   = json.find('"', valueBegin);
        if (valueEnd == std::string_view::npos) {
            return {};
        }
        return std::string(json.substr(valueBegin, valueEnd - valueBegin));
    }

    class ScriptHostPlugin final : public mcdk::Plugin {
    public:
        // 在任何阶段回调之前被调用，此时 config 已可读。
        mcdk::PluginIdentity identity(mcdk::Context& context) override {
            const auto config = context.configJson();
            mScript           = field(config, "script");

            mcdk::PluginIdentity result;
            result.id   = field(config, "id");
            result.name = field(config, "name");
            // version 留空 → 沿用 MCDK_PLUGIN 里的静态值。
            return result;
        }

        void onRegister(mcdk::Context& context) override {
            if (mScript.empty()) {
                // 加载器拿不到脚本就没有存在意义，直接让本实例加载失败。
                // 抛出的异常由 SDK 屏障转成状态码，宿主据此卸载这一个实例，
                // 其余声明不受影响。
                throw mcdk::Error(MCDK_ERR_INVALID_ARGUMENT, "config.script is required");
            }
            context.console().info("loader:register:" + mScript);
            // 真实实现在这里创建解释器、执行脚本、把脚本注册的 MCP 工具
            // 转发到 ctx.mcp()。
        }

        void onRuntime(mcdk::Context& context) override { context.console().info("loader:runtime:" + mScript); }

        void onShutdown(mcdk::Context& context) override {
            context.console().info("loader:shutdown:" + mScript);
            // 真实实现在这里销毁解释器。注意 on_unload 返回前必须 join 掉
            // 脚本可能创建的线程（03-abi-reference.md §5.2）。
        }

    private:
        std::string mScript;
    };

} // namespace

MCDK_PLUGIN(ScriptHostPlugin, "com.example.script-host", "0.1.0");
