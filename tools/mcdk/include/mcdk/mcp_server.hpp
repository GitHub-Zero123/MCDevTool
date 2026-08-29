#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include <mcdk/settings.hpp>

namespace mcdk {

    class LogBuffer;

    // 供 mcdk_instance_info 工具对外描述"这一路 MCP 背后是哪个游戏"，多开时用于区分实例。
    struct McpInstanceInfo {
        std::uint32_t         mcdkPid = 0;
        std::filesystem::path projectRoot;
        std::string           worldName;
        std::string           worldFolderName;
        std::string           startedAt;
    };

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
        void setInstanceInfo(McpInstanceInfo info);

        [[nodiscard]] int getMinecraftProcessId() const;

        // 实际绑定成功的端口；未启动或区间内无可用端口时为 0。
        [[nodiscard]] int getBoundPort() const;

        // 在配置的端口区间内依次尝试绑定，成功后服务运行于 getBoundPort()。
        void start();
        void stop();

    private:
        class Impl;
        std::unique_ptr<Impl> mImpl;
    };

} // namespace mcdk
