#pragma once
// mcdk.world.resolve：插件改写存档设置。契约见 04-events.md §4.5。
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <mcdk/plugin/abi/core.h>
#include <mcdk/settings.hpp>

namespace mcdk::plugin_host {

    struct WorldOverride {
        std::string        ownerId;
        bool               replaced = false; // 完全覆盖 / 按键覆盖
        WorldProjectConfig world;
    };

    // 发 mcdk.world.resolve。有插件改写就返回改写后的设置，没有返回 nullopt。
    // 插件否决时抛异常：它要求中止启动。
    [[nodiscard]] std::optional<WorldOverride> resolveWorld(const WorldProjectConfig& current);

    // override_world 的实现体，由 mcdk.game 的 shim 调用。
    [[nodiscard]] mcdk_status
    overrideWorld(mcdk_handle owner, mcdk_handle request, mcdk_str settingsJson, std::uint32_t mode);

    // 运行时存档目录名：单层、非空、不含路径分隔符与 Windows 保留字符。
    // reset_world 会整个删掉该目录，放过 ".." 就等于让插件删任意目录。
    [[nodiscard]] bool isValidWorldFolderName(std::string_view name) noexcept;

} // namespace mcdk::plugin_host
