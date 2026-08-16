#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace mcdk {

    namespace GameEnvironmentVariables {
        inline constexpr auto DebugOptions = L"MCDEV_DEBUG_OPTIONS";
        inline constexpr auto TargetModDirs = L"MCDEV_TARGET_MOD_DIRS";
        inline constexpr auto DebugIpcPort = L"MCDEV_DEBUG_IPC_PORT";
        inline constexpr auto LogProtocol = L"MCDEV_LOG_PROTOCOL";
    } // namespace GameEnvironmentVariables

    class GameEnvironmentBuilder final {
    public:
        GameEnvironmentBuilder& set(std::wstring_view name, std::wstring value);
        GameEnvironmentBuilder& setUtf8(std::wstring_view name, std::string_view value);

        [[nodiscard]] std::wstring build() &&;

    private:
        std::vector<std::wstring> mEntries;
    };

} // namespace mcdk
