#include <env.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string_view>

namespace mcdk {
    namespace {
        int readPort(const char* variableName) {
            const auto* value = std::getenv(variableName);
            if (value == nullptr) {
                return 0;
            }
            try {
                const int port = std::stoi(value);
                return port > 0 && port <= 65535 ? port : 0;
            } catch (...) {
                return 0;
            }
        }

        bool readBoolean(const char* variableName) {
            const auto* value = std::getenv(variableName);
            if (value == nullptr) {
                return false;
            }
            std::string normalized(value);
            std::ranges::transform(normalized, normalized.begin(), [](unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            return normalized == "1" || normalized == "true" || normalized == "yes";
        }
    } // namespace

    int getEnvOutputMode() {
        static const int outputMode = [] {
            const auto* value = std::getenv("MCDEV_OUTPUT_MODE");
            if (value == nullptr) {
                return 0;
            }
            try {
                return std::stoi(value);
            } catch (...) {
                return 0;
            }
        }();
        return outputMode;
    }

    int getEnvDebuggerPort() { return readPort("MCDEV_MODPC_DEBUGGER_PORT"); }

    bool getEnvIsSubprocessMode() { return readBoolean("MCDEV_IS_SUBPROCESS_MODE"); }

    bool getEnvIsPluginEnv() { return readBoolean("MCDEV_IS_PLUGIN_ENV"); }

    int getEnvAutoJoinGameState() {
        const auto* value = std::getenv("MCDEV_AUTO_JOIN_GAME");
        if (value == nullptr) {
            return -1;
        }
        const std::string_view setting(value);
        if (setting == "0" || setting == "false" || setting == "False") {
            return 0;
        }
        if (setting == "1" || setting == "true" || setting == "True") {
            return 1;
        }
        return -1;
    }

    int getEnvNeteaseDebugPort() { return readPort("MCDEV_NETEASE_DEBUG_PORT"); }

    std::string getEnvNeteaseDebugPortStr() {
        const auto* value = std::getenv("MCDEV_NETEASE_DEBUG_PORT");
        return value == nullptr ? std::string{} : std::string(value);
    }

    std::string getEnvPtvsdIp() {
        const auto* value = std::getenv("MCDEV_PTVSD_IP");
        return value == nullptr ? std::string{} : std::string(value);
    }

    int getEnvPtvsdPort() { return readPort("MCDEV_PTVSD_PORT"); }

    PtvsdConfig getEnvPtvsdConfig() {
        PtvsdConfig config;
        const auto  ip   = getEnvPtvsdIp();
        const int   port = getEnvPtvsdPort();
        if (!ip.empty() && port > 0) {
            config.enabled = true;
            config.ip      = ip;
            config.port    = port;
        }
        return config;
    }

    PtvsdConfig resolvePtvsdConfig(const PtvsdConfig& userConfig) {
        auto environmentConfig = getEnvPtvsdConfig();
        return environmentConfig.enabled ? environmentConfig : userConfig;
    }

    std::string buildPtvsdLaunchArgs(const PtvsdConfig& config) {
        if (!config.enabled) {
            return {};
        }
        return "debug_ip=" + config.ip + " debug_port=" + std::to_string(config.port);
    }

} // namespace mcdk
