#pragma once

#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include <mcdk/runtime/mcp_tool_registry.hpp>
#include <mcdk/settings.hpp>

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

        // 工具注册表。内置工具与插件工具共用这张表，start() 时按表发布。
        [[nodiscard]] const std::shared_ptr<runtime::McpToolRegistry>& toolRegistry() const;
        // 把全部内置工具注册进注册表。幂等。
        // 调用方应在给插件放行注册之前调用它，让内置工具先占位；之后封存注册表，
        void registerBuiltinTools();

        // 封存注册表并把表中全部工具发布给 MCP 服务器，随后非阻塞启动。
        void start();
        void stop();

    private:
        class Impl;
        std::unique_ptr<Impl> mImpl;
    };

} // namespace mcdk
