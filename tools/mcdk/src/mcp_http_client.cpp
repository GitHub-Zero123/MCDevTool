#include <mcdk/mcp_http_client.hpp>

#include <utility>

#include <httplib.h>
#include <mcp_message.h>

namespace mcdk {
    namespace {
        constexpr auto Endpoint = "/mcp";

        void configure(httplib::Client& client, int connectSeconds, int readSeconds) {
            client.set_connection_timeout(connectSeconds, 0);
            client.set_read_timeout(readSeconds, 0);
            client.set_write_timeout(readSeconds, 0);
        }
    } // namespace

    McpHttpClient::McpHttpClient(Options options) : mOptions(std::move(options)) {}

    void McpHttpClient::disconnect() {
        mConnected = false;
        mSessionId.clear();
    }

    void McpHttpClient::close() {
        if (!mSessionId.empty()) {
            httplib::Client client(baseUrl());
            configure(client, mOptions.connectTimeoutSeconds, mOptions.initializeTimeoutSeconds);
            client.Delete(Endpoint, httplib::Headers{{"Mcp-Session-Id", mSessionId}});
        }
        disconnect();
    }

    std::string McpHttpClient::baseUrl() const {
        return "http://" + mOptions.host + ":" + std::to_string(mOptions.port);
    }

    std::string McpHttpClient::endpointUrl() const { return baseUrl() + Endpoint; }

    bool McpHttpClient::initialize(std::string& error) {
        httplib::Client client(baseUrl());
        configure(client, mOptions.connectTimeoutSeconds, mOptions.initializeTimeoutSeconds);

        const Json request = {
            {"jsonrpc", "2.0"},
            {"id", nextId()},
            {"method", "initialize"},
            {"params",
             {{"protocolVersion", mcp::MCP_VERSION},
              {"capabilities", Json::object()},
              {"clientInfo", {{"name", mOptions.clientName}, {"version", mOptions.clientVersion}}}}}
        };

        const auto result = client.Post(Endpoint, request.dump(), "application/json");
        if (!result) {
            error = "cannot connect to the MCDK game MCP endpoint";
            return false;
        }
        if (result->status / 100 != 2) {
            error = "MCDK game MCP returned HTTP " + std::to_string(result->status);
            return false;
        }

        const auto response = Json::parse(result->body, nullptr, false);
        if (response.is_discarded() || response.contains("error")) {
            error = "MCDK game MCP initialization failed";
            return false;
        }

        const auto sessionHeader = result->headers.find("Mcp-Session-Id");
        if (sessionHeader == result->headers.end() || sessionHeader->second.empty()) {
            error = "MCDK game MCP did not provide a session id";
            return false;
        }

        mSessionId = sessionHeader->second;
        mConnected = true;

        Json        ignored;
        std::string ignoredError;
        post(Json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}, ignored, ignoredError);
        return true;
    }

    bool McpHttpClient::ping() {
        Json        response;
        std::string error;
        const bool  ok = post(Json{{"jsonrpc", "2.0"}, {"id", nextId()}, {"method", "ping"}}, response, error);
        return ok && !response.contains("error");
    }

    bool McpHttpClient::post(const Json& request, Json& response, std::string& error) {
        httplib::Client client(baseUrl());
        configure(client, mOptions.connectTimeoutSeconds, mOptions.requestTimeoutSeconds);

        httplib::Headers headers;
        headers.emplace("Content-Type", "application/json");
        if (!mSessionId.empty()) {
            headers.emplace("Mcp-Session-Id", mSessionId);
        }

        const auto result = client.Post(Endpoint, headers, request.dump(), "application/json");
        if (!result) {
            error = "cannot connect to the MCDK game MCP endpoint";
            return false;
        }
        if (request.contains("id") && result->status / 100 != 2) {
            error = "MCDK game MCP returned HTTP " + std::to_string(result->status);
            return false;
        }
        if (!request.contains("id")) {
            response = Json::object();
            return true;
        }

        response = Json::parse(result->body, nullptr, false);
        if (response.is_discarded()) {
            error = "MCDK game MCP returned invalid JSON";
            return false;
        }
        return true;
    }

} // namespace mcdk
