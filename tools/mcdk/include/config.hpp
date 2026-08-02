#pragma once

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

#include "settings.hpp"

namespace mcdk {

    [[nodiscard]] std::optional<std::filesystem::path>
    selectGameExePath(const std::vector<std::filesystem::path>& paths);

    [[nodiscard]] UserConfig createDefaultConfig();
    [[nodiscard]] UserConfig parseUserConfig(std::string_view jsonText);
    [[nodiscard]] UserConfig userParseConfig();

    [[nodiscard]] bool updateGamePath(std::filesystem::path& path);
    void               tryUpdateUserGamePath(const std::filesystem::path& newPath);

} // namespace mcdk
