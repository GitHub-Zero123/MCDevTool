#pragma once

#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "settings.hpp"

namespace mcdk {

    class LogBuffer;

    class MCPServer {
    public:
        using CodeExecuteHandler =
            std::function<nlohmann::json(const std::string& code, bool isClient, bool directReturn)>;
        using ProfilerHandler = std::function<nlohmann::json(const nlohmann::json& arguments)>;
        using SimpleHandler    = std::function<bool()>;
        using BoolParamHandler = std::function<bool(bool parameter)>;

        explicit MCPServer(const McpServerConfig& config);
        explicit MCPServer(McpServerConfig&& config);
        ~MCPServer();

        MCPServer(MCPServer&&) noexcept;
        MCPServer& operator=(MCPServer&&) noexcept;
        MCPServer(const MCPServer&)            = delete;
        MCPServer& operator=(const MCPServer&) = delete;

        void setLogBuffer(std::shared_ptr<LogBuffer> buffer);
        void setErrBuffer(std::shared_ptr<LogBuffer> buffer);
        void setCodeExecuteHandler(CodeExecuteHandler handler);
        void setProfilerHandler(ProfilerHandler handler);
        void setReloadGameHandler(BoolParamHandler handler);
        void setReloadUiHandler(SimpleHandler handler);
        void setMinecraftProcessId(int processId);

        [[nodiscard]] int getMinecraftProcessId() const;

        void start();
        void stop();

    private:
        class Impl;
        std::unique_ptr<Impl> mImpl;
    };

} // namespace mcdk
