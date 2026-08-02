#include <ipc_code_execution.hpp>

#include <nlohmann/json.hpp>

namespace mcdk::ipc_code_execution {

    nlohmann::json requestCodeReturnValueJson(
        const std::shared_ptr<MCDevTool::Debug::DebugIPCServer>& ipcServer,
        const std::string&                                       code,
        bool                                                     isClient,
        uint32_t                                                 timeoutMs
    ) {
        if (!ipcServer) {
            return {{"ok", false}, {"error", "IPC server is null"}};
        }

        const nlohmann::json params = {
            {"code", code},
            {"is_client", isClient},
            {"timeout", static_cast<double>(timeoutMs) / 1000.0},
        };
        const auto result = ipcServer->requestJson("execute_code", params.dump(), timeoutMs);
        if (!result.success) {
            return {{"ok", false}, {"error", result.errorMessage}};
        }

        auto response = nlohmann::json::parse(result.responseJson, nullptr, false);
        if (response.is_discarded() || !response.is_object() || !response.value("ok", false)) {
            return {
                {"ok", false},
                {"error", "invalid execute_code response"},
                {"response", result.responseJson},
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
        return {
            {"ok", false},
            {"error", "execute_code response has no return_value"},
            {"response", response},
        };
    }

    nlohmann::json requestClientCodeReturnValueJson(
        const std::shared_ptr<MCDevTool::Debug::DebugIPCServer>& ipcServer,
        const std::string&                                       code,
        uint32_t                                                 timeoutMs
    ) {
        return requestCodeReturnValueJson(ipcServer, code, true, timeoutMs);
    }

} // namespace mcdk::ipc_code_execution
