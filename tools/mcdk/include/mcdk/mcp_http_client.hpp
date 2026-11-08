#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace mcdk {

    // 连接 mcdk 内置 MCP 的 Streamable HTTP 客户端，stdio bridge 与 CLI 共用。
    class McpHttpClient {
    public:
        using Json = nlohmann::ordered_json;

        struct Options {
            std::string host;
            int         port = 0;
            std::string clientName;
            std::string clientVersion;
            int         connectTimeoutSeconds    = 1;
            int         initializeTimeoutSeconds = 3;
            int         requestTimeoutSeconds    = 30;
        };

        explicit McpHttpClient(Options options);

        [[nodiscard]] bool connected() const noexcept { return mConnected; }
        void               disconnect();
        // 通知服务端结束会话再断开；不调用时服务端要等闲置超时才回收。
        void               close();
        [[nodiscard]] int  nextId() noexcept { return mNextId++; }

        [[nodiscard]] std::string baseUrl() const;
        [[nodiscard]] std::string endpointUrl() const;

        // 失败时把原因写进 error。
        bool initialize(std::string& error);
        bool ping();
        // 不带 id 的通知不解析响应，response 置空对象。
        bool post(const Json& request, Json& response, std::string& error);

    private:
        Options     mOptions;
        std::string mSessionId;
        bool        mConnected = false;
        int         mNextId    = 1;
    };

} // namespace mcdk
