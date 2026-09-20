//
// ABI 一致性插件。
//
// 它不是给用户抄的示例，而是 docs/plugin-system/09-compatibility.md §2 要求的
// 验证载荷：故意在各个回调点抛出不同类型的异常，用来证明 SDK 的屏障确实把它们
// 拦在了 C ABI 边界内侧，且宿主的行为符合 02-abi-contract.md §4.2 的失败语义表。
//
// 行为完全由 .mcdev.json 里这条声明的 config 决定，因此同一个 DLL 可以被声明
// 多次、各自覆盖一条路径 —— 顺便也验证了「可传参式插件」确实是多实例的。
//
//   { "tag": "reg", "throw_at": "register", "throw_kind": "std" }
//
// throw_at:   register / config / world / runtime / shutdown，缺省或其他值表示不抛
// throw_kind: std（std::runtime_error）/ custom（自定义类型）/ int（非 std 派生）
//
#include <mcdk/plugin/plugin.hpp>

#include <stdexcept>
#include <string>
#include <string_view>

namespace {

    // 朴素取值，避免给 SDK 示例引入 JSON 依赖。测试完全控制 config 的写法。
    [[nodiscard]] std::string_view field(std::string_view json, std::string_view key) {
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
        return json.substr(valueBegin, valueEnd - valueBegin);
    }

    struct CustomError {
        int code = 0;
    };

    class ConformancePlugin final : public mcdk::Plugin {
    public:
        void onRegister(mcdk::Context& context) override { visit(context, "register"); }
        void onConfig(mcdk::Context& context) override { visit(context, "config"); }
        void onWorld(mcdk::Context& context) override { visit(context, "world"); }
        void onRuntime(mcdk::Context& context) override { visit(context, "runtime"); }
        void onShutdown(mcdk::Context& context) override { visit(context, "shutdown"); }

    private:
        void visit(mcdk::Context& context, std::string_view stage) {
            const auto config = context.configJson();
            const auto tag    = field(config, "tag");

            // 每个实例带着自己的 tag 打日志：若 SDK 用了静态单例，多次声明会
            // 互相覆盖，这里就会打出同一个 tag，测试立刻发现。
            context.console().info("conformance:" + std::string(tag) + ":" + std::string(stage));

            if (field(config, "throw_at") != stage) {
                return;
            }
            const auto kind = field(config, "throw_kind");
            if (kind == "custom") {
                // 不用指派初始化：SDK 的下限是 C++17，插件不该被迫跟到 C++20。
                throw CustomError{7};
            }
            if (kind == "int") {
                // 非 std::exception 派生，只有 catch(...) 兜得住。
                throw 42;
            }
            throw std::runtime_error("deliberate failure at " + std::string(stage));
        }
    };

} // namespace

MCDK_PLUGIN(ConformancePlugin, "com.mcdev.abi-conformance", "0.1.0");
