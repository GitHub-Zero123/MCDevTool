#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <mcdevtool/debug.h>
#include <nlohmann/json_fwd.hpp>

namespace mcdk::ipc_code_execution {

    [[nodiscard]] nlohmann::json requestCodeReturnValueJson(
        const std::shared_ptr<MCDevTool::Debug::DebugIPCServer>& ipcServer,
        const std::string&                                       code,
        bool                                                     isClient,
        uint32_t                                                 timeoutMs = 10000
    );

    [[nodiscard]] nlohmann::json requestClientCodeReturnValueJson(
        const std::shared_ptr<MCDevTool::Debug::DebugIPCServer>& ipcServer,
        const std::string&                                       code,
        uint32_t                                                 timeoutMs = 10000
    );

} // namespace mcdk::ipc_code_execution
