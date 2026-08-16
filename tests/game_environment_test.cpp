#include <mcdk/game_environment.hpp>

#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {
    void require(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    std::map<std::wstring, std::wstring> parseEnvironmentBlock(const std::wstring& block) {
        require(block.size() >= 2, "Environment block is too small.");
        require(block[block.size() - 1] == L'\0', "Environment block is not NUL terminated.");
        require(block[block.size() - 2] == L'\0', "Environment block is not double-NUL terminated.");

        std::map<std::wstring, std::wstring> values;
        for (const auto* current = block.data(); *current != L'\0';) {
            const std::wstring_view entry(current);
            const auto separator = entry.find(L'=', entry.starts_with(L'=') ? 1U : 0U);
            require(separator != std::wstring_view::npos, "Environment entry has no name separator.");
            values.emplace(entry.substr(0, separator), entry.substr(separator + 1));
            current += entry.size() + 1;
        }
        return values;
    }
}

int main() {
    SetEnvironmentVariableW(L"MCDEV_HOST_PORT", L"19132");
    SetEnvironmentVariableW(L"MCDEV_HOST_TOKEN", L"secret");
    SetEnvironmentVariableW(L"mcdev_debug_ipc_port", L"65535");
    SetEnvironmentVariableW(L"MCDK_ENVIRONMENT_TEST_INHERITED", L"kept");

    mcdk::GameEnvironmentBuilder environment;
    environment.setUtf8(mcdk::GameEnvironmentVariables::DebugOptions, R"({"reload_key":"192","name":"热更新"})");
    environment.setUtf8(mcdk::GameEnvironmentVariables::TargetModDirs, R"(["D:/模组/src"])");
    environment.set(mcdk::GameEnvironmentVariables::DebugIpcPort, L"43210");

    const auto values = parseEnvironmentBlock(std::move(environment).build());
    require(!values.contains(L"MCDEV_HOST_PORT"), "Host port leaked into the game environment.");
    require(!values.contains(L"MCDEV_HOST_TOKEN"), "Host token leaked into the game environment.");
    require(!values.contains(L"mcdev_debug_ipc_port"), "Inherited IPC port was not replaced case-insensitively.");
    require(values.at(L"MCDK_ENVIRONMENT_TEST_INHERITED") == L"kept", "Parent environment was not inherited.");
    require(values.at(L"MCDEV_DEBUG_IPC_PORT") == L"43210", "IPC port override is incorrect.");
    require(
        values.at(L"MCDEV_DEBUG_OPTIONS") == LR"({"reload_key":"192","name":"热更新"})",
        "UTF-8 debug options conversion is incorrect."
    );
    require(
        values.at(L"MCDEV_TARGET_MOD_DIRS") == LR"(["D:/模组/src"])",
        "UTF-8 Mod directory conversion is incorrect."
    );

    mcdk::GameEnvironmentBuilder withoutIpc;
    const auto valuesWithoutIpc = parseEnvironmentBlock(std::move(withoutIpc).build());
    require(!valuesWithoutIpc.contains(L"MCDEV_DEBUG_IPC_PORT"), "Unexpected uppercase IPC port was inherited.");
    require(!valuesWithoutIpc.contains(L"mcdev_debug_ipc_port"), "Unexpected lowercase IPC port was inherited.");

    return 0;
}
