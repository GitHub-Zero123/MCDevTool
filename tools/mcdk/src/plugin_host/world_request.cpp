#include <mcdk/plugin_host/world_request.hpp>

#include <mcdk/config.hpp>
#include <mcdk/plugin/abi/iface/game.h>
#include <mcdk/plugin_host/events.hpp>
#include <mcdk/plugin_host/guard.hpp>
#include <mcdk/plugin_host/process_request.hpp>
#include <mcdk/world_project.hpp>

#include "registry.hpp"

#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace mcdk::plugin_host {

    namespace {

        struct State {
            std::mutex                   mutex;
            mcdk_handle                  active = 0;
            mcdk_handle                  next   = 1;
            const WorldProjectConfig*    current = nullptr;
            std::optional<WorldOverride> result;
        };

        State& state() {
            static State instance;
            return instance;
        }

        // 受理窗口只活在 resolveWorld 的一次派发里。
        class Window {
        public:
            explicit Window(const WorldProjectConfig& current) {
                const std::lock_guard lock(state().mutex);
                if (state().active != 0) {
                    throw std::logic_error("mcdk.world.resolve 的受理窗口不可重入");
                }
                mHandle          = state().next++;
                state().active   = mHandle;
                state().current  = &current;
                state().result.reset();
            }

            ~Window() {
                const std::lock_guard lock(state().mutex);
                state().active  = 0;
                state().current = nullptr;
                state().result.reset();
            }

            Window(const Window&)            = delete;
            Window& operator=(const Window&) = delete;

            [[nodiscard]] mcdk_handle handle() const noexcept { return mHandle; }

            [[nodiscard]] bool settled() const {
                const std::lock_guard lock(state().mutex);
                return state().result.has_value();
            }

            [[nodiscard]] std::optional<WorldOverride> take() {
                const std::lock_guard lock(state().mutex);
                return std::exchange(state().result, std::nullopt);
            }

        private:
            mcdk_handle mHandle = 0;
        };

    } // namespace

    bool isValidWorldFolderName(std::string_view name) noexcept {
        if (name.empty() || name == "." || name == ".." || name.back() == '.' || name.back() == ' ') {
            return false;
        }
        for (const char c : name) {
            if (static_cast<unsigned char>(c) < 0x20 || std::string_view("/\\:*?\"<>|").find(c) != std::string_view::npos) {
                return false;
            }
        }
        return true;
    }

    std::optional<WorldOverride> resolveWorld(const WorldProjectConfig& current) {
        std::optional<Window> window;
        const auto            verdict = MCDK_EMIT_UNTIL(
            EventId::WorldResolve,
            [&] { return window->settled(); },
            [&](auto& arena) {
                window.emplace(current);
                mcdk_ev_world_resolve payload{};
                payload.struct_size = static_cast<std::uint32_t>(sizeof(payload));
                payload.request     = window->handle();
                payload.world_json  = arena.hold(worldConfigToJson(current).dump());
                return payload;
            }
        );
        auto result = window ? window->take() : std::nullopt;
        if (!result && verdict.result == MCDK_EVENT_VETO) {
            throw std::runtime_error("插件 " + pluginIdOf(verdict.owner) + " 否决了存档设置，启动中止");
        }
        return result;
    }

    mcdk_status overrideWorld(mcdk_handle owner, mcdk_handle request, mcdk_str settingsJson, std::uint32_t mode) {
        const auto* record = detail::registry().find(owner);
        if (record == nullptr) {
            return MCDK_ERR_INVALID_HANDLE;
        }
        std::string ownerId = record->id;

        const std::lock_guard lock(state().mutex);
        if (request == 0 || request != state().active) {
            return setError(MCDK_ERR_INVALID_HANDLE, "request 已失效：只能在 mcdk.world.resolve 的回调里改写");
        }
        if (state().result) {
            return setError(MCDK_ERR_DUPLICATE, "存档设置已由插件 " + state().result->ownerId + " 改写");
        }
        if (mode != MCDK_WORLD_OVERRIDE_MERGE && mode != MCDK_WORLD_OVERRIDE_REPLACE) {
            return setError(MCDK_ERR_INVALID_ARGUMENT, "未知的改写模式 " + std::to_string(mode));
        }

        const std::string_view text(settingsJson.ptr != nullptr ? settingsJson.ptr : "", settingsJson.len);
        const auto             settings = nlohmann::json::parse(text, nullptr, false);
        if (settings.is_discarded() || !settings.is_object()) {
            return setError(MCDK_ERR_INVALID_ARGUMENT, "settings_json 必须是 JSON 对象");
        }
        for (const auto& [key, value] : settings.items()) {
            if (!isWorldConfigKey(key)) {
                return setError(MCDK_ERR_INVALID_ARGUMENT, "未知的存档设置键：" + key);
            }
        }

        WorldOverride result;
        result.ownerId  = std::move(ownerId);
        result.replaced = mode == MCDK_WORLD_OVERRIDE_REPLACE;
        result.world    = result.replaced ? WorldProjectConfig{} : *state().current;
        try {
            applyWorldConfig(settings, result.world);
            // 地图目录在这里就验掉，插件当场拿到错误，而不是等到启动中途。
            (void)resolveWorldSourcePath(result.world.source);
        } catch (const std::exception& error) {
            return setError(MCDK_ERR_INVALID_ARGUMENT, error.what());
        }
        if (!isValidWorldFolderName(result.world.folderName)) {
            return setError(
                MCDK_ERR_INVALID_ARGUMENT,
                "world_folder_name 必须是单层目录名，不能含路径分隔符或 ..：" + result.world.folderName
            );
        }
        state().result = std::move(result);
        return MCDK_OK;
    }

} // namespace mcdk::plugin_host
