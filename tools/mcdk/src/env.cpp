#include <mcdk/env.hpp>

#include <algorithm>
#include <charconv>
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

        bool isHexToken(std::string_view token) {
            return token.size() == 64 && std::ranges::all_of(token, [](const unsigned char character) {
                return std::isxdigit(character) != 0;
            });
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

    int getEnvDebuggerPort() {
        static const int port = readPort("MCDEV_MODPC_DEBUGGER_PORT");
        return port;
    }

    bool getEnvIsSubprocessMode() {
        static const bool enabled = readBoolean("MCDEV_IS_SUBPROCESS_MODE");
        return enabled;
    }

    bool getEnvIsPluginEnv() {
        static const bool enabled = readBoolean("MCDEV_IS_PLUGIN_ENV");
        return enabled;
    }

    int getEnvAutoJoinGameState() {
        static const int state = [] {
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
        }();
        return state;
    }

    int getEnvNeteaseDebugPort() {
        static const int port = readPort("MCDEV_NETEASE_DEBUG_PORT");
        return port;
    }

    std::string getEnvNeteaseDebugPortStr() {
        static const std::string port = [] {
            const auto* value = std::getenv("MCDEV_NETEASE_DEBUG_PORT");
            return value == nullptr ? std::string{} : std::string(value);
        }();
        return port;
    }

    std::string getEnvPtvsdIp() {
        static const std::string ip = [] {
            const auto* value = std::getenv("MCDEV_PTVSD_IP");
            return value == nullptr ? std::string{} : std::string(value);
        }();
        return ip;
    }

    int getEnvPtvsdPort() {
        static const int port = readPort("MCDEV_PTVSD_PORT");
        return port;
    }

    const HostBridgeConfig& getEnvHostBridgeConfig() {
        static const HostBridgeConfig config = [] {
            HostBridgeConfig result;
            const auto*      portValue = std::getenv("MCDEV_HOST_PORT");
            if (portValue == nullptr || *portValue == '\0') {
                return result;
            }
            result.configured = true;

            int         parsedPort = 0;
            const auto* end        = portValue + std::char_traits<char>::length(portValue);
            const auto  parsed     = std::from_chars(portValue, end, parsedPort);
            if (parsed.ec != std::errc{} || parsed.ptr != end || parsedPort <= 0 || parsedPort > 65535) {
                result.errorMessage = "MCDEV_HOST_PORT must be an integer in 1..65535";
                return result;
            }

            const auto* tokenValue = std::getenv("MCDEV_HOST_TOKEN");
            if (tokenValue == nullptr || !isHexToken(tokenValue)) {
                result.errorMessage = "MCDEV_HOST_TOKEN must contain exactly 64 hexadecimal characters";
                return result;
            }

            result.enabled = true;
            result.port    = static_cast<std::uint16_t>(parsedPort);
            result.token   = tokenValue;
            return result;
        }();
        return config;
    }

    PtvsdConfig getEnvPtvsdConfig() {
        static const PtvsdConfig config = [] {
            PtvsdConfig result;
            const auto  ip   = getEnvPtvsdIp();
            const int   port = getEnvPtvsdPort();
            if (!ip.empty() && port > 0) {
                result.enabled = true;
                result.ip      = ip;
                result.port    = port;
            }
            return result;
        }();
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
