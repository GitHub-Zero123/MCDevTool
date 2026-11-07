// mcdk.core/1 的宿主实现。
// 本文件里的每个导出函数都必须经过 guardVoid / guard，没有例外
#include <string>

#include <mcdk/plugin/abi/iface/core.h>
#include <mcdk/plugin_host/guard.hpp>

#include "../registry.hpp"

namespace mcdk::plugin_host::detail {

    namespace {

        [[nodiscard]] mcdk_str toAbi(std::string_view text) noexcept {
            mcdk_str out;
            out.ptr = text.data();
            out.len = text.size();
            return out;
        }

        void MCDK_CALL coreGetLastError(mcdk_handle self, mcdk_str* out_message) noexcept {
            guardVoid([&] {
                if (out_message == nullptr) {
                    return;
                }
                *out_message = mcdk_str{};
                if (registry().find(self) == nullptr) {
                    return;
                }
                // 指向宿主的线程局部缓冲，调用方须立即拷贝。
                *out_message = toAbi(errorSlot().message);
            });
        }

        void MCDK_CALL coreGetHostVersion(mcdk_handle self, mcdk_str* out_version) noexcept {
            guardVoid([&] {
                if (out_version == nullptr) {
                    return;
                }
                *out_version = mcdk_str{};
                if (registry().find(self) == nullptr) {
                    return;
                }
                *out_version = toAbi(hostVersion());
            });
        }

        mcdk_stage MCDK_CALL coreGetStage(mcdk_handle self) noexcept {
            mcdk_stage stage = MCDK_STAGE_REGISTER;
            guardVoid([&] {
                if (registry().find(self) == nullptr) {
                    return;
                }
                stage = currentStage();
            });
            return stage;
        }

        void MCDK_CALL coreGetConfig(mcdk_handle self, mcdk_str* out_config_json) noexcept {
            guardVoid([&] {
                if (out_config_json == nullptr) {
                    return;
                }
                // 未设置时也是合法 JSON，插件无需特判。
                static const std::string kNull = "null";
                *out_config_json               = toAbi(kNull);

                const auto* record = registry().find(self);
                if (record == nullptr) {
                    return;
                }
                // 文本由宿主持有，生命周期覆盖整个会话；仍按借用语义交付。
                *out_config_json = toAbi(record->configJson);
            });
        }

        constexpr mcdk_iface_core kTable = {
            /* struct_size      */ static_cast<uint32_t>(sizeof(mcdk_iface_core)),
            /* _reserved        */ 0u,
            /* get_last_error   */ &coreGetLastError,
            /* get_host_version */ &coreGetHostVersion,
            /* get_stage        */ &coreGetStage,
            /* get_config       */ &coreGetConfig,
        };

    } // namespace

    const mcdk_iface_core* coreTable() noexcept { return &kTable; }

} // namespace mcdk::plugin_host::detail
