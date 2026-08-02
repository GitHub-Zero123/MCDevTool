#pragma once

#include <string>

#include "settings.hpp"

namespace mcdk {

    [[nodiscard]] int         getEnvOutputMode();
    [[nodiscard]] int         getEnvDebuggerPort();
    [[nodiscard]] bool        getEnvIsSubprocessMode();
    [[nodiscard]] bool        getEnvIsPluginEnv();
    [[nodiscard]] int         getEnvAutoJoinGameState();
    [[nodiscard]] int         getEnvNeteaseDebugPort();
    [[nodiscard]] std::string getEnvNeteaseDebugPortStr();
    [[nodiscard]] std::string getEnvPtvsdIp();
    [[nodiscard]] int         getEnvPtvsdPort();

    [[nodiscard]] PtvsdConfig getEnvPtvsdConfig();
    [[nodiscard]] PtvsdConfig resolvePtvsdConfig(const PtvsdConfig& userConfig);
    [[nodiscard]] std::string buildPtvsdLaunchArgs(const PtvsdConfig& config);

} // namespace mcdk
