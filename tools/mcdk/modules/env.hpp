#pragma once
#include <string>
#include <algorithm>
#include <cstdlib>

namespace mcdk {

    // 获取环境变量MCDEV_OUTPUT_MODE的输出策略：0.默认，视为终端处理  1.视为vscode/pycharm的调试伪终端处理
    inline int getEnvOutputMode() {
        static int outputMode = -1;
        if(outputMode != -1) {
            return outputMode;
        }
        auto* modeStr = std::getenv("MCDEV_OUTPUT_MODE");
        if(modeStr == nullptr) {
            outputMode = 0;
        } else {
            try {
                outputMode = std::stoi(modeStr);
            } catch(...) {
                outputMode = 0;
            }
        }
        return outputMode;
    }

    // 获取环境变量 强制修改调试器端口号
    inline int getEnvDebuggerPort() {
        auto* portStr = std::getenv("MCDEV_MODPC_DEBUGGER_PORT");
        if(portStr == nullptr) {
            return 0;
        }
        try {
            int port = std::stoi(portStr);
            if(port > 0 && port <= 65535) {
                return port;
            }
        } catch(...) {}
        return 0;
    }

    // 获取环境变量 是否是子进程模式
    inline bool getEnvIsSubprocessMode() {
        auto* valStr = std::getenv("MCDEV_IS_SUBPROCESS_MODE");
        if(valStr == nullptr) {
            return false;
        }
        std::string val(valStr);
        std::transform(val.begin(), val.end(), val.begin(), ::tolower);
        return (val == "1" || val == "true" || val == "yes");
    }

    // 获取环境变量 是否为插件环境
    inline bool getEnvIsPluginEnv() {
        auto* valStr = std::getenv("MCDEV_IS_PLUGIN_ENV");
        if(valStr == nullptr) {
            return false;
        }
        std::string val(valStr);
        std::transform(val.begin(), val.end(), val.begin(), ::tolower);
        return (val == "1" || val == "true" || val == "yes");
    }

    // 获取环境变量强制覆写的自动进入游戏存档状态 -1.未设置 0.关闭 1.开启
    inline int getEnvAutoJoinGameState() {
        auto* autoJoin = std::getenv("MCDEV_AUTO_JOIN_GAME");
        if(autoJoin == nullptr) {
            return -1;
        }
        std::string_view valStr(autoJoin);
        if(valStr == "0" || valStr == "false" || valStr == "False") {
            return 0;
        } else if(valStr == "1" || valStr == "true" || valStr == "True") {
            return 1;
        }
        return -1;
    }

    // 获取网易自定义调试端口 MCDEV_NETEASE_DEBUG_PORT变量
    inline int getEnvNeteaseDebugPort() {
        auto* portStr = std::getenv("MCDEV_NETEASE_DEBUG_PORT");
        if(portStr == nullptr) {
            return 0;
        }
        try {
            int port = std::stoi(portStr);
            if(port > 0 && port <= 65535) {
                return port;
            }
        } catch(...) {}
        return 0;
    }

    // getEnvNeteaseDebugPort的字符串版本
    inline std::string getEnvNeteaseDebugPortStr() {
        auto* portStr = std::getenv("MCDEV_NETEASE_DEBUG_PORT");
        if(portStr == nullptr) {
            return "";
        }
        return std::string(portStr);
    }

} // namespace mcdk
