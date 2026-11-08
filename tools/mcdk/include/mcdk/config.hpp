#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include <mcdk/settings.hpp>

namespace mcdk {

    [[nodiscard]] std::optional<std::filesystem::path>
    selectGameExePath(const std::vector<std::filesystem::path>& paths);

    [[nodiscard]] UserConfig createDefaultConfig();
    [[nodiscard]] UserConfig parseUserConfig(std::string_view jsonText);
    [[nodiscard]] UserConfig userParseConfig();
    // 读取指定配置并依次应用 overrides。不创建、不写回、不询问，失败抛异常。
    [[nodiscard]] UserConfig
    loadUserConfig(const std::filesystem::path& path, const std::vector<std::string>& overrides = {});
    // 把 "a.b=value" 写进 root。value 能解析成 JSON 就取解析结果，否则按字符串。
    void applyConfigOverride(nlohmann::json& root, std::string_view expression);

    // 存档相关的键（world_name、reset_world、game_mode……），.mcdev.json 与插件改写共用。
    [[nodiscard]] bool isWorldConfigKey(std::string_view key) noexcept;

    // mcdk 读取的全部顶层配置键。
    [[nodiscard]] std::vector<std::string> knownConfigKeys();
    // 覆写项里既不是已知配置键、也不在配置文件里的顶层键，多半是拼错了。
    [[nodiscard]] std::vector<std::string>
    unknownOverrideKeys(const std::filesystem::path& path, const std::vector<std::string>& overrides);
    // 只改 source 里出现的存档键，其余保持 world 原值。类型不对时抛异常。
    void applyWorldConfig(const nlohmann::json& source, WorldProjectConfig& world);
    // 全部存档键的当前值。
    [[nodiscard]] nlohmann::json worldConfigToJson(const WorldProjectConfig& world);

    [[nodiscard]] bool updateGamePath(std::filesystem::path& path);
    void               tryUpdateUserGamePath(const std::filesystem::path& newPath);

} // namespace mcdk
