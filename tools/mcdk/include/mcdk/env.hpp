#pragma once

#include <cstdint>
#include <string>

#include <mcdk/settings.hpp>

namespace mcdk {

    struct HostBridgeConfig {
        bool          configured = false;
        bool          enabled    = false;
        std::uint16_t port       = 0;
        std::string   token;
        std::string   errorMessage;
    };

    [[nodiscard]] int         getEnvOutputMode();
    [[nodiscard]] int         getEnvDebuggerPort();
    [[nodiscard]] bool        getEnvIsSubprocessMode();
    [[nodiscard]] bool        getEnvIsPluginEnv();
    [[nodiscard]] int         getEnvAutoJoinGameState();
    [[nodiscard]] int         getEnvNeteaseDebugPort();
    [[nodiscard]] std::string getEnvNeteaseDebugPortStr();
    [[nodiscard]] std::string getEnvPtvsdIp();
    [[nodiscard]] int         getEnvPtvsdPort();
    [[nodiscard]] const HostBridgeConfig& getEnvHostBridgeConfig();

    [[nodiscard]] PtvsdConfig getEnvPtvsdConfig();
    [[nodiscard]] PtvsdConfig resolvePtvsdConfig(const PtvsdConfig& userConfig);
    [[nodiscard]] std::string buildPtvsdLaunchArgs(const PtvsdConfig& config);

} // namespace mcdk
