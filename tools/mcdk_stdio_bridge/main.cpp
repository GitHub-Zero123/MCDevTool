#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#endif

#include "../mcdk/modules/mcp_tool_definitions.hpp"

#include <httplib.h>
#include <mcp_message.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cctype>
#include <chrono>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

#ifdef _WIN32
    void configureStdioAndUtf8Console() {
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
        SetConsoleCP(CP_UTF8);
        SetConsoleOutputCP(CP_UTF8);
        std::setlocale(LC_ALL, ".UTF-8");
    }
#else
    void configureStdioAndUtf8Console() { std::setlocale(LC_ALL, ""); }
#endif

    using json = nlohmann::ordered_json;

    constexpr const char* BridgeName               = "Minecraft(BE) MCP Stdio Bridge(MCDK)";
    constexpr const char* BridgeVersion            = "0.1.0";
    constexpr const char* DefaultHost              = "localhost";
    constexpr int         DefaultPort              = 19133;
    constexpr const char* StreamableEndpoint       = "/mcp";
    constexpr int         ConnectTimeoutSeconds    = 1;
    constexpr int         ReadWriteTimeoutSeconds  = 30;
    constexpr int         InitializationTimeoutSec = 3;

    struct BridgeConfig {
        std::string host = DefaultHost;
        int         port = DefaultPort;
    };

    std::string trim(std::string_view value) {
        auto begin = value.begin();
        auto end   = value.end();
        while (begin != end && std::isspace(static_cast<unsigned char>(*begin))) {
            ++begin;
        }
        while (begin != end && std::isspace(static_cast<unsigned char>(*(end - 1)))) {
            --end;
        }
        return std::string(begin, end);
    }

    bool parseInteger(const std::string& value, int& out) {
        try {
            size_t index = 0;
            int    port  = std::stoi(value, &index);
            if (index != value.size() || port <= 0 || port > 65535) {
                return false;
            }
            out = port;
            return true;
        } catch (...) {
            return false;
        }
    }

    BridgeConfig parseArgs(int argc, char** argv) {
        BridgeConfig config;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i] ? argv[i] : "";
            auto readNext = [&](std::string& target) {
                if (i + 1 < argc) {
                    target = argv[++i] ? argv[i] : "";
                }
            };

            if (arg == "--host" || arg == "-h") {
                readNext(config.host);
            } else if (arg.rfind("--host=", 0) == 0) {
                config.host = arg.substr(7);
            } else if (arg == "--port" || arg == "-p") {
                std::string portText;
                readNext(portText);
                int parsedPort = config.port;
                if (parseInteger(portText, parsedPort)) {
                    config.port = parsedPort;
                }
            } else if (arg.rfind("--port=", 0) == 0) {
                int parsedPort = config.port;
                if (parseInteger(arg.substr(7), parsedPort)) {
                    config.port = parsedPort;
                }
            } else {
                int parsedPort = config.port;
                if (parseInteger(arg, parsedPort)) {
                    config.port = parsedPort;
                }
            }
        }
        if (config.host.empty()) {
            config.host = DefaultHost;
        }
        return config;
    }

    json makeTextContent(const std::string& text) {
        return json::array({{{"type", "text"}, {"text", text}}});
    }

    json makeToolErrorResult(const std::string& text) {
        return json{{"isError", true}, {"content", makeTextContent(text)}};
    }

    json makeErrorResponse(const json& id, mcp::error_code code, const std::string& message) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", static_cast<int>(code)}, {"message", message}}}};
    }

    json makeSuccessResponse(const json& id, const json& result) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
    }

    std::string dumpJsonReplacingInvalidUtf8(const json& value) {
        return value.dump(-1, ' ', false, json::error_handler_t::replace);
    }

    class StdioTransport {
    public:
        std::optional<json> readMessage() {
            std::string firstLine;
            if (!std::getline(std::cin, firstLine)) {
                return std::nullopt;
            }
            if (!firstLine.empty() && firstLine.back() == '\r') {
                firstLine.pop_back();
            }
            if (firstLine.empty()) {
                return std::nullopt;
            }

            std::string headerKey = firstLine.substr(0, firstLine.find(':'));
            for (auto& c : headerKey) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }

            if (headerKey == "content-length") {
                auto colon = firstLine.find(':');
                if (colon == std::string::npos) {
                    return std::nullopt;
                }

                size_t contentLength = 0;
                try {
                    contentLength = static_cast<size_t>(std::stoull(trim(std::string_view(firstLine).substr(colon + 1))));
                } catch (...) {
                    return std::nullopt;
                }

                std::string headerLine;
                while (std::getline(std::cin, headerLine)) {
                    if (!headerLine.empty() && headerLine.back() == '\r') {
                        headerLine.pop_back();
                    }
                    if (headerLine.empty()) {
                        break;
                    }
                }

                std::string body(contentLength, '\0');
                std::cin.read(body.data(), static_cast<std::streamsize>(contentLength));
                if (std::cin.gcount() != static_cast<std::streamsize>(contentLength)) {
                    return std::nullopt;
                }
                return json::parse(body, nullptr, false);
            }

            return json::parse(firstLine, nullptr, false);
        }

        void writeMessage(const json& message) {
            std::lock_guard<std::mutex> lock(writeMutex_);
            std::cout << dumpJsonReplacingInvalidUtf8(message) << '\n';
            std::cout.flush();
        }

    private:
        std::mutex writeMutex_;
    };

    class GameMcpClient {
    public:
        explicit GameMcpClient(BridgeConfig config) : config_(std::move(config)) {}

        json callTool(const std::string& name, const json& arguments) {
            std::string error;
            if (!ensureConnected(error)) {
                return makeToolErrorResult(error);
            }

            json response;
            if (!postJson(json{{"jsonrpc", "2.0"}, {"id", nextId_++}, {"method", "tools/call"}, {"params", {{"name", name}, {"arguments", arguments}}}}, response, error)) {
                connected_ = false;
                sessionId_.clear();
                if (!ensureConnected(error)) {
                    return makeToolErrorResult(error);
                }
                if (!postJson(json{{"jsonrpc", "2.0"}, {"id", nextId_++}, {"method", "tools/call"}, {"params", {{"name", name}, {"arguments", arguments}}}}, response, error)) {
                    connected_ = false;
                    sessionId_.clear();
                    return makeToolErrorResult(error);
                }
            }

            if (response.contains("error")) {
                const auto& err = response["error"];
                return makeToolErrorResult(
                    "MCDK game MCP returned an error: " + err.value("message", dumpJsonReplacingInvalidUtf8(response))
                );
            }
            if (!response.contains("result")) {
                return makeToolErrorResult(
                    "MCDK game MCP returned an invalid response: " + dumpJsonReplacingInvalidUtf8(response)
                );
            }
            return response["result"];
        }

    private:
        bool ensureConnected(std::string& error) {
            if (connected_ && ping()) {
                return true;
            }
            connected_ = false;
            sessionId_.clear();
            return initialize(error);
        }

        bool initialize(std::string& error) {
            httplib::Client client(baseUrl());
            configureClient(client, InitializationTimeoutSec);

            json initializeRequest = {
                {"jsonrpc", "2.0"},
                {"id", nextId_++},
                {"method", "initialize"},
                {"params",
                 {{"protocolVersion", mcp::MCP_VERSION},
                  {"capabilities", json::object()},
                  {"clientInfo", {{"name", "MCDKStdioBridge"}, {"version", BridgeVersion}}}}}
            };

            auto result = client.Post(StreamableEndpoint, initializeRequest.dump(), "application/json");
            if (!result) {
                error = notReadyMessage("cannot connect to the MCDK game MCP endpoint");
                return false;
            }
            if (result->status / 100 != 2) {
                error = notReadyMessage("MCDK game MCP returned HTTP " + std::to_string(result->status));
                return false;
            }

            json response = json::parse(result->body, nullptr, false);
            if (response.is_discarded() || response.contains("error")) {
                error = notReadyMessage("MCDK game MCP initialization failed");
                return false;
            }

            auto sessionHeader = result->headers.find("Mcp-Session-Id");
            if (sessionHeader == result->headers.end() || sessionHeader->second.empty()) {
                error = notReadyMessage("MCDK game MCP did not provide a session id");
                return false;
            }

            sessionId_ = sessionHeader->second;
            connected_ = true;

            json initializedNotification = {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
            json ignored;
            std::string ignoredError;
            postJson(initializedNotification, ignored, ignoredError);
            return true;
        }

        bool ping() {
            json        response;
            std::string error;
            bool ok = postJson(json{{"jsonrpc", "2.0"}, {"id", nextId_++}, {"method", "ping"}}, response, error);
            return ok && !response.contains("error");
        }

        bool postJson(const json& request, json& response, std::string& error) {
            httplib::Client client(baseUrl());
            configureClient(client, ReadWriteTimeoutSeconds);

            httplib::Headers headers;
            headers.emplace("Content-Type", "application/json");
            if (!sessionId_.empty()) {
                headers.emplace("Mcp-Session-Id", sessionId_);
            }

            auto result = client.Post(StreamableEndpoint, headers, request.dump(), "application/json");
            if (!result) {
                error = notReadyMessage("cannot connect to the MCDK game MCP endpoint");
                return false;
            }
            if (request.contains("id") && result->status / 100 != 2) {
                error = notReadyMessage("MCDK game MCP returned HTTP " + std::to_string(result->status));
                return false;
            }
            if (!request.contains("id")) {
                response = json::object();
                return true;
            }

            response = json::parse(result->body, nullptr, false);
            if (response.is_discarded()) {
                error = notReadyMessage("MCDK game MCP returned invalid JSON");
                return false;
            }
            return true;
        }

        void configureClient(httplib::Client& client, int readTimeoutSeconds) const {
            client.set_connection_timeout(ConnectTimeoutSeconds, 0);
            client.set_read_timeout(readTimeoutSeconds, 0);
            client.set_write_timeout(readTimeoutSeconds, 0);
        }

        std::string baseUrl() const { return "http://" + config_.host + ":" + std::to_string(config_.port); }

        std::string notReadyMessage(const std::string& detail) const {
            return "Minecraft has not been launched through MCDK, or MCDK MCP is not enabled/configured. "
                   "Please start the game with MCDK and enable mcp_server_config first. Target endpoint: "
                + baseUrl() + StreamableEndpoint + ". Detail: " + detail;
        }

        BridgeConfig config_;
        std::string  sessionId_;
        bool         connected_ = false;
        int          nextId_    = 1;
    };

    class BridgeServer {
    public:
        explicit BridgeServer(BridgeConfig config) : gameClient_(std::move(config)) {}

        void run() {
            StdioTransport transport;
            while (true) {
                auto message = transport.readMessage();
                if (!message.has_value()) {
                    break;
                }
                if (message->is_discarded()) {
                    continue;
                }
                auto response = handleMessage(*message);
                if (response.has_value()) {
                    transport.writeMessage(*response);
                }
            }
        }

    private:
        std::optional<json> handleMessage(const json& message) {
            if (!message.is_object() || !message.contains("method")) {
                json id = message.is_object() && message.contains("id") ? message["id"] : nullptr;
                return makeErrorResponse(id, mcp::error_code::invalid_request, "Invalid JSON-RPC request");
            }

            const bool isNotification = !message.contains("id") || message["id"].is_null();
            json       id             = isNotification ? nullptr : message["id"];
            std::string method         = message.value("method", "");
            json        params         = message.value("params", json::object());

            if (isNotification) {
                return std::nullopt;
            }

            if (method == "initialize") {
                return makeSuccessResponse(
                    id,
                    json{
                        {"protocolVersion", mcp::MCP_VERSION},
                        {"capabilities", {{"tools", json::object()}}},
                        {"serverInfo", {{"name", BridgeName}, {"version", BridgeVersion}}}
                    }
                );
            }
            if (method == "ping") {
                return makeSuccessResponse(id, json::object());
            }
            if (method == "tools/list") {
                json tools = json::array();
                for (const auto& tool : mcdk::mcp_tool_definitions::buildAllTools()) {
                    tools.push_back(tool.to_json());
                }
                return makeSuccessResponse(id, json{{"tools", tools}});
            }
            if (method == "tools/call") {
                if (!params.is_object() || !params.contains("name")) {
                    return makeErrorResponse(id, mcp::error_code::invalid_params, "Missing 'name' parameter");
                }
                std::string toolName  = params.value("name", "");
                json        arguments = params.value("arguments", json::object());
                return makeSuccessResponse(id, gameClient_.callTool(toolName, arguments));
            }
            if (method == "resources/list") {
                return makeSuccessResponse(id, json{{"resources", json::array()}});
            }
            if (method == "resources/templates/list") {
                return makeSuccessResponse(id, json{{"resourceTemplates", json::array()}});
            }
            if (method == "prompts/list") {
                return makeSuccessResponse(id, json{{"prompts", json::array()}});
            }

            return makeErrorResponse(id, mcp::error_code::method_not_found, "Method not found: " + method);
        }

        GameMcpClient gameClient_;
    };

} // namespace

int main(int argc, char** argv) {
    configureStdioAndUtf8Console();
    BridgeServer server(parseArgs(argc, argv));
    server.run();
    return 0;
}
