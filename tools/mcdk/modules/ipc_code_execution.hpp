#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <mcdevtool/debug.h>
#include <nlohmann/json.hpp>

namespace mcdk::ipc_code_execution {

    inline nlohmann::json requestCodeReturnValueJson(
        const std::shared_ptr<MCDevTool::Debug::DebugIPCServer>& ipcServer,
        const std::string&                                       code,
        bool                                                     isClient,
        uint32_t                                                 timeoutMs = 10000
    ) {
        if (!ipcServer) {
            return nlohmann::json{
                {"ok", false},
                {"error", "IPC server is null"}
            };
        }

        nlohmann::json params = {
            {"code", code},
            {"is_client", isClient},
            {"timeout", static_cast<double>(timeoutMs) / 1000.0}
        };
        auto result = ipcServer->requestJson("execute_code", params.dump(), timeoutMs);
        if (!result.success) {
            return nlohmann::json{
                {"ok", false},
                {"error", result.errorMessage}
            };
        }

        auto response = nlohmann::json::parse(result.responseJson, nullptr, false);
        if (response.is_discarded() || !response.is_object() || !response.value("ok", false)) {
            return nlohmann::json{
                {"ok", false},
                {"error", "invalid execute_code response"},
                {"response", result.responseJson}
            };
        }

        auto payload = response.value("result", nlohmann::json::object());
        if (payload.is_object() && payload.contains("return_value")) {
            const auto& returnValue = payload["return_value"];
            if (returnValue.is_string()) {
                auto nested = nlohmann::json::parse(returnValue.get<std::string>(), nullptr, false);
                if (!nested.is_discarded()) {
                    return nested;
                }
            }
            return returnValue;
        }

        return nlohmann::json{
            {"ok", false},
            {"error", "execute_code response has no return_value"},
            {"response", response}
        };
    }

    inline nlohmann::json requestClientCodeReturnValueJson(
        const std::shared_ptr<MCDevTool::Debug::DebugIPCServer>& ipcServer,
        const std::string&                                       code,
        uint32_t                                                 timeoutMs = 10000
    ) {
        return requestCodeReturnValueJson(ipcServer, code, true, timeoutMs);
    }

} // namespace mcdk::ipc_code_execution
