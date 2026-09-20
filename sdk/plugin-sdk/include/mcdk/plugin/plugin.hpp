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
#include "events.hpp"
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
// 这些是正常的 C++ 虚函数；SDK 会为它们生成带异常屏障的 noexcept 蹦床。
    class Plugin {
    public:
        Plugin()                         = default;
        virtual ~Plugin()                = default;
        Plugin(const Plugin&)            = delete;
        Plugin& operator=(const Plugin&) = delete;
// 在任何阶段回调之前调用，此时 Context 的 config 已可读。
// 返回的非空字段会覆盖 MCDK_PLUGIN 宏里写死的对应值。
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

        // 由 SDK 的入口胶水调用，插件不应直接使用。
        void bindContext(Context& context) noexcept { mContext = &context; }

    protected:
        // 本插件的 Context。
        // 与各阶段回调收到的是**同一个对象**：一次加载只有一个 Context，从入口
        [[nodiscard]] Context& context() noexcept { return *mContext; }

    private:
        // 在第一次回调（含 identity）之前就已经被绑好，不会是空的。
        Context* mContext = nullptr;
    };

    namespace detail {
// 一次加载对应的全部插件侧状态。
// 刻意不用静态单例：同一动态库可在 .mcdev.json 中声明多次。
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
                // 出错时返回 MCDK_ERR_PLUGIN_EXCEPTION；异常在 guard 内被吃掉。
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
                // 实例由插件侧分配，也在这里释放。
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
                    // 宿主头文件必须不早于插件版本，否则所需接口可能不存在。
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
                    // 在任何回调之前绑定，包括下一行的 identity()——这样 Plugin::context()
                    // 在插件能观察到的任何时刻都是有效的。
                    instance->plugin.bindContext(instance->context);
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
// class MyPlugin final : public mcdk::Plugin { ... };
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
