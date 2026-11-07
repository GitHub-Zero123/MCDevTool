// 改写存档设置示例：把游戏引到编辑器专用的存档，不碰用户 .mcdev.json 里的那个。
// 定制版编辑器通常配合 MCDEV_PLUGINS 注入它（06-loading.md §2.4）。
#include <mcdk/plugin/plugin.hpp>

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

    [[nodiscard]] std::string quoted(std::string_view text) {
        std::string out = "\"";
        for (const char c : text) {
            if (c == '"' || c == '\\') {
                out += '\\';
            }
            out += c;
        }
        return out + "\"";
    }

    class WorldPlugin final : public mcdk::Plugin {
    public:
        void onRegister(mcdk::Context& context) override {
            const auto config = context.configJson();
            mFolder           = field(config, "folder");
            mReset            = field(config, "reset") == "true";
            mMode = field(config, "mode") == "replace" ? mcdk::WorldOverride::Replace : mcdk::WorldOverride::Merge;
            if (mFolder.empty()) {
                mFolder = "MCDK_EDITOR_WORLD";
            }

            context.events().on<mcdk::ev::WorldResolve>([this, &context](const auto& e) {
                context.console().info("world:current:" + field(e.worldJson, "world_folder_name"));

                // 只写要改的键。Merge 下其余沿用用户的设置，Replace 下其余回到默认值。
                const std::string settings = "{\"world_folder_name\":" + quoted(mFolder)
                                           + ",\"world_name\":" + quoted(mFolder)
                                           + ",\"reset_world\":" + (mReset ? "true" : "false") + "}";
                const auto status = context.game().overrideWorld(e.request, settings, mMode);
                context.console().info("world:override:" + std::to_string(status));
                if (status != MCDK_OK) {
                    // 进不了编辑器存档就别启动，免得在用户的存档里乱改。
                    context.console().error("world:error:" + context.lastHostError());
                    return mcdk::EventResult::Veto;
                }
                return mcdk::EventResult::Stop;
            });
        }

    private:
        std::string         mFolder;
        bool                mReset = false;
        mcdk::WorldOverride mMode  = mcdk::WorldOverride::Merge;
    };

} // namespace

MCDK_PLUGIN(WorldPlugin, "com.example.editor-world", "0.1.0");
