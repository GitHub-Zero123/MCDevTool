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

        // 每个插件类型一份的静态状态。插件是单例，宿主一个进程只加载一次。
        template <class PluginT>
        struct PluginBootstrap {
            static inline Context                  context;
            static inline std::unique_ptr<PluginT> instance;

            static void MCDK_CALL onStage(void* /*user*/, mcdk_stage stage) noexcept {
                guardVoid([stage] {
                    if (!instance) {
                        return;
                    }
                    switch (stage) {
                    case MCDK_STAGE_REGISTER:
                        instance->onRegister(context);
                        break;
                    case MCDK_STAGE_CONFIG:
                        instance->onConfig(context);
                        break;
                    case MCDK_STAGE_WORLD:
                        instance->onWorld(context);
                        break;
                    case MCDK_STAGE_RUNTIME:
                        instance->onRuntime(context);
                        break;
                    case MCDK_STAGE_SHUTDOWN:
                        instance->onShutdown(context);
                        break;
                    default:
                        // 新宿主可能推进旧插件不认识的阶段，忽略即可，不是错误。
                        break;
                    }
                });
            }

            static void MCDK_CALL onUnload(void* /*user*/) noexcept {
                guardVoid([] { instance.reset(); });
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

                    context.bindHost(*host);
                    instance = std::make_unique<PluginT>();

                    *out             = mcdk_plugin_desc{};
                    out->struct_size = static_cast<uint32_t>(sizeof(mcdk_plugin_desc));
                    out->abi_major   = MCDK_ABI_VERSION_MAJOR;
                    out->abi_minor   = MCDK_ABI_VERSION_MINOR;
                    out->min_stage   = MCDK_STAGE_REGISTER;
                    // 这些都是静态存储期的字面量，宿主可以安全地长期持有。
                    out->id        = toAbi(std::string_view(id));
                    out->name      = toAbi(std::string_view(name));
                    out->version   = toAbi(std::string_view(version));
                    out->user      = nullptr;
                    out->on_stage  = &onStage;
                    out->on_unload = &onUnload;

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
