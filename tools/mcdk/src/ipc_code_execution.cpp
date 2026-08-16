#include <mcdk/ipc_code_execution.hpp>

#include <utility>

#include <nlohmann/json.hpp>

namespace mcdk::ipc_code_execution {

    nlohmann::json requestCodeReturnValueJson(
        const std::shared_ptr<MCDevTool::Debug::DebugIPCServer>& ipcServer,
        std::string                                              code,
        bool                                                     isClient,
        uint32_t                                                 timeoutMs
    ) {
        if (!ipcServer) {
            return {{"ok", false}, {"error", "IPC server is null"}};
        }

        nlohmann::json params = {
            // Callers usually generate code as a temporary; move it directly into the wire JSON without a deep copy.
            {"code", std::move(code)},
            {"is_client", isClient},
            {"timeout", static_cast<double>(timeoutMs) / 1000.0},
        };
        auto result = ipcServer->requestJsonValue("execute_code", std::move(params), timeoutMs);
        if (!result.success) {
            return {{"ok", false}, {"error", result.errorMessage}};
        }

        // The IPC read thread already parsed this response to route its id; move that DOM into the consumer.
        auto response = result.responseValue
                      ? std::move(*result.responseValue)
                      : nlohmann::json::parse(result.responseJson, nullptr, false);
        if (response.is_discarded() || !response.is_object() || !response.value("ok", false)) {
            return {
                {"ok", false},
                {"error", "invalid execute_code response"},
                {"response", std::move(result.responseJson)},
            };
        }
        // The parsed tree now owns the useful data; release the duplicate wire string before handling large results.
        result.responseJson.clear();

        auto resultValue = response.find("result");
        if (resultValue != response.end() && resultValue->is_object() && resultValue->contains("return_value")) {
            auto returnValue = std::move((*resultValue)["return_value"]);
            if (returnValue.is_string()) {
                const auto& serialized = returnValue.get_ref<const std::string&>();
                auto nested = nlohmann::json::parse(serialized, nullptr, false);
                if (!nested.is_discarded()) {
                    return nested;
                }
            }
            return returnValue;
        }
        return {
            {"ok", false},
            {"error", "execute_code response has no return_value"},
            {"response", std::move(response)},
        };
    }

    nlohmann::json requestClientCodeReturnValueJson(
        const std::shared_ptr<MCDevTool::Debug::DebugIPCServer>& ipcServer,
        std::string                                              code,
        uint32_t                                                 timeoutMs
    ) {
        return requestCodeReturnValueJson(ipcServer, std::move(code), true, timeoutMs);
    }

} // namespace mcdk::ipc_code_execution
