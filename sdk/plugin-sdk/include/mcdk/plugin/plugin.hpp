#pragma once

//
// 插件作者需要 include 的唯一头文件。
//

#include <memory>
#include <type_traits>

#include "abi/core.h"
#include "abi/entry.h"
#include "console.hpp"
#include "context.hpp"
#include "detail/abi_bridge.hpp"
#include "detail/barrier.hpp"
#include "error.hpp"

namespace mcdk {

    // 插件向宿主报告的身份。任一字段留空表示沿用 MCDK_PLUGIN 宏里写死的值。
    struct PluginIdentity {
        std::string id;
        std::string name;
        std::string version;
    };

    // 插件基类。全部回调都有空实现，只重写用得上的那些。
    //
    // 这些是正常的 C++ 虚函数；SDK 的 MCDK_PLUGIN 宏会为它们生成 noexcept 的
    // 静态蹦床并装上异常屏障，填进 mcdk_plugin_desc 这张纯 C 函数指针表。
    // 边界上只剩 void* 与函数指针——手法与 godot-cpp 的 GDCLASS 宏相同。
    class Plugin {
    public:
        Plugin()                         = default;
        virtual ~Plugin()                = default;
        Plugin(const Plugin&)            = delete;
        Plugin& operator=(const Plugin&) = delete;

        // 在任何阶段回调之前调用，此时 Context 的 config 已可读。
        // 返回的非空字段会覆盖 MCDK_PLUGIN 宏里写死的对应值。
        //
        // 这是「一个 DLL 充当其他插件的加载器」的关键。一个 Python / Lua 绑定
        // 宿主在 .mcdev.json 里被声明多次、每条 config 指向不同脚本，此时每个
        // 实例必须报出属于那个脚本的身份——否则五个脚本在宿主眼里是同一个
        // 插件：日志分不清是谁打的，声明里的 id 防替换校验失效，将来的重名
        // 检测与依赖解析也一并失效。
        //
        //   PluginIdentity identity(Context& ctx) override {
        //       const auto script = parseScriptName(ctx.configJson());
        //       return {.id = "com.me.py." + script, .name = script, .version = "1.0.0"};
        //   }
        //
        // 普通插件不必重写：身份是编译期固定的。
        virtual PluginIdentity identity(Context& context) {
            (void)context;
            return {};
        }

        // 只能注册：事件、MCP 工具。
        virtual void onRegister(Context& context) { (void)context; }
        // UserConfig 已解析。
        virtual void onConfig(Context& context) { (void)context; }
        // 世界目录与 pack manifest 已就绪。
        virtual void onWorld(Context& context) { (void)context; }
        // 运行期子系统均已就绪，游戏进程已启动。
        virtual void onRuntime(Context& context) { (void)context; }
        // 即将终结。返回前必须 join 自己创建的全部线程。
        virtual void onShutdown(Context& context) { (void)context; }
    };

    namespace detail {

        // 一次加载对应的全部插件侧状态。
        //
        // 刻意不用静态单例：宿主允许同一个动态库在 .mcdev.json 里被声明多次、
        // 各带不同的 config（「可传参式插件」）。此时入口会被调用多次，每次都
        // 必须得到自己的 Context 与插件对象，否则后一次会把前一次覆盖掉。
        //
        // 实例指针放进 mcdk_plugin_desc::user，由宿主原样回传给每个回调——
        // 这正是该字段存在的意义。所有权随之交给宿主，在 on_unload 中释放。
        template <class PluginT>
        struct PluginInstance {
            Context context;
            PluginT plugin;
            // 动态身份的字符串必须活在实例里：mcdk_plugin_desc 中的 mcdk_str
            // 指向它们，宿主在入口返回后立刻深拷贝，而实例此时已交给宿主。
            PluginIdentity identity;
        };

        template <class PluginT>
        struct PluginBootstrap {
            using Instance = PluginInstance<PluginT>;

            static mcdk_status MCDK_CALL onStage(void* user, mcdk_stage stage) noexcept {
                // 出错时返回 MCDK_ERR_PLUGIN_EXCEPTION，宿主据此判定该插件在本阶段
                // 失败。异常本身在 guard 内被吃掉，绝不穿越边界。
                return guard(
                    [user, stage]() -> mcdk_status {
                        auto* instance = static_cast<Instance*>(user);
                        if (instance == nullptr) {
                            return MCDK_ERR_INVALID_HANDLE;
                        }
                        switch (stage) {
                        case MCDK_STAGE_REGISTER:
                            instance->plugin.onRegister(instance->context);
                            break;
                        case MCDK_STAGE_CONFIG:
                            instance->plugin.onConfig(instance->context);
                            break;
                        case MCDK_STAGE_WORLD:
                            instance->plugin.onWorld(instance->context);
                            break;
                        case MCDK_STAGE_RUNTIME:
                            instance->plugin.onRuntime(instance->context);
                            break;
                        case MCDK_STAGE_SHUTDOWN:
                            instance->plugin.onShutdown(instance->context);
                            break;
                        default:
                            // 新宿主可能推进旧插件不认识的阶段，忽略即可，不是错误。
                            break;
                        }
                        return MCDK_OK;
                    },
                    MCDK_ERR_PLUGIN_EXCEPTION
                );
            }

            static void MCDK_CALL onUnload(void* user) noexcept {
                // 实例是在插件自己的堆上 new 出来的，也在这里 delete——
                // 跨界的只有这个不透明指针，分配器不穿越边界（02 §6）。
                guardVoid([user] { delete static_cast<Instance*>(user); });
            }

            // 入口函数的实际实现。
            static mcdk_bool enter(
                const mcdk_host_info* host,
                mcdk_plugin_desc*     out,
                const char*           id,
                const char*           name,
                const char*           version
            ) noexcept {
                mcdk_bool ok = MCDK_FALSE;
                guardVoid([&] {
                    if (host == nullptr || out == nullptr) {
                        return;
                    }
                    // struct_size / abi_major / abi_minor 三个字段的偏移永久冻结，
                    // 因此可以在校验版本之前安全地读它们。
                    if (host->struct_size < 3 * sizeof(uint32_t)) {
                        return;
                    }
                    if (host->abi_major != MCDK_ABI_VERSION_MAJOR) {
                        return;
                    }
                    // 宿主必须至少和本插件编译时的头一样新，否则本插件用到的
                    // 字段可能不存在。宿主侧会做对称的检查并给出可读提示。
                    if (host->abi_minor < MCDK_ABI_VERSION_MINOR) {
                        return;
                    }
                    if (host->struct_size < sizeof(mcdk_host_info)) {
                        return;
                    }

                    // 本次加载独占的状态。构造期间抛异常由外层 guardVoid 吃掉，
                    // unique_ptr 保证此时不泄漏。
                    auto instance = std::make_unique<Instance>();
                    instance->context.bindHost(*host);
                    // config 此时已就绪，插件可以据此报出动态身份（加载器场景）。
                    instance->identity = instance->plugin.identity(instance->context);

                    // 留空的字段沿用 MCDK_PLUGIN 宏里的静态字面量。
                    const auto pick = [](const std::string& dynamic, const char* fallback) -> std::string_view {
                        return dynamic.empty() ? std::string_view(fallback) : std::string_view(dynamic);
                    };

                    *out             = mcdk_plugin_desc{};
                    out->struct_size = static_cast<uint32_t>(sizeof(mcdk_plugin_desc));
                    out->abi_major   = MCDK_ABI_VERSION_MAJOR;
                    out->abi_minor   = MCDK_ABI_VERSION_MINOR;
                    out->min_stage   = MCDK_STAGE_REGISTER;
                    // 指向静态字面量或实例内的字符串，两者都活过入口返回，
                    // 宿主随后立即深拷贝。
                    out->id        = toAbi(pick(instance->identity.id, id));
                    out->name      = toAbi(pick(instance->identity.name, name));
                    out->version   = toAbi(pick(instance->identity.version, version));
                    out->on_stage  = &onStage;
                    out->on_unload = &onUnload;
                    // 填完描述才移交所有权：前面任何一步失败都不会留下孤儿实例。
                    out->user = instance.release();

                    ok = MCDK_TRUE;
                });
                return ok;
            }
        };

    } // namespace detail

} // namespace mcdk

// 声明插件。放在插件的某个 .cpp 文件里，一个动态库一次。
//
//   class MyPlugin final : public mcdk::Plugin { ... };
//   MCDK_PLUGIN(MyPlugin, "com.example.my-plugin", "1.0.0")
//
#define MCDK_PLUGIN(PluginClass, PluginId, PluginVersion)                                                              \
    extern "C" MCDK_PLUGIN_EXPORT mcdk_bool MCDK_CALL                                                                  \
    mcdk_plugin_entry(const mcdk_host_info* host, mcdk_plugin_desc* out_desc) {                                        \
        return ::mcdk::detail::PluginBootstrap<PluginClass>::enter(                                                    \
            host,                                                                                                      \
            out_desc,                                                                                                  \
            (PluginId),                                                                                                \
            #PluginClass,                                                                                              \
            (PluginVersion)                                                                                            \
        );                                                                                                             \
    }                                                                                                                  \
    static_assert(                                                                                                     \
        ::std::is_base_of_v<::mcdk::Plugin, PluginClass>,                                                              \
        #PluginClass " passed to MCDK_PLUGIN must derive from mcdk::Plugin"                                            \
    )
