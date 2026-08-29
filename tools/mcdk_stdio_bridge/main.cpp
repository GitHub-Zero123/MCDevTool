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

#include <mcdk/mcp_tool_definitions.hpp>
#include <mcdk/mc_profiler_mcp.hpp>
#include <mcdk/port_range.hpp>

#include "discovery.hpp"

#include <httplib.h>
#include <mcp_message.h>
#include <mcp_tool.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
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
    constexpr const char* BridgeVersion            = "0.2.0";
    constexpr const char* DefaultHost              = "localhost";
    constexpr const char* StreamableEndpoint       = "/mcp";
    constexpr int         ConnectTimeoutSeconds    = 1;
    constexpr int         ReadWriteTimeoutSeconds  = 30;
    constexpr int         InitializationTimeoutSec = 3;

    constexpr const char* ListInstancesToolName = "mcdk_instances";
    constexpr const char* UseInstanceToolName   = "mcdk_use";

    struct BridgeConfig {
        std::string     host = DefaultHost;
        mcdk::PortRange ports{mcdk::DefaultMcpPort, mcdk::DefaultMcpPortRangeEnd};
        // 用户显式给了单值端口时锁定该端口，行为与旧版本完全一致：不做发现，直连。
        bool            lockedToSinglePort = false;
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

    bool isAllDigits(std::string_view text) {
        return !text.empty() && text.find_first_not_of("0123456789") == std::string_view::npos;
    }

    // 解析端口参数，同时兼容旧的单值写法与新的区间写法。
    void applyPortArgument(std::string_view text, BridgeConfig& config) {
        const auto parsed = mcdk::parsePortRange(text, config.ports);
        if (!parsed.ok) {
            std::cerr << "[mcdk_stdio_bridge] " << parsed.error << "，继续使用 " << config.ports.toString() << "\n";
            return;
        }
        if (!parsed.warning.empty()) {
            std::cerr << "[mcdk_stdio_bridge] " << parsed.warning << "\n";
        }
        config.ports              = parsed.range;
        config.lockedToSinglePort = parsed.range.isSingle();
    }

    BridgeConfig parseArgs(int argc, char** argv) {
        BridgeConfig config;
        for (int i = 1; i < argc; ++i) {
            const std::string arg      = argv[i] != nullptr ? argv[i] : "";
            const auto        readNext = [&](std::string& target) {
                if (i + 1 < argc) {
                    target = argv[++i] != nullptr ? argv[i] : "";
                }
            };

            if (arg == "--host" || arg == "-h") {
                readNext(config.host);
            } else if (arg.rfind("--host=", 0) == 0) {
                config.host = arg.substr(7);
            } else if (arg == "--port" || arg == "-p" || arg == "--ports" || arg == "--port-range") {
                std::string portText;
                readNext(portText);
                // --port-range / --ports 也允许写成两个独立参数：--port-range 19133 19142
                if (arg != "--port" && arg != "-p" && isAllDigits(trim(portText)) && i + 1 < argc
                    && argv[i + 1] != nullptr && isAllDigits(trim(argv[i + 1]))) {
                    portText = trim(portText) + "-" + trim(argv[i + 1]);
                    ++i;
                }
                applyPortArgument(portText, config);
            } else if (arg.rfind("--port=", 0) == 0) {
                applyPortArgument(arg.substr(7), config);
            } else if (arg.rfind("--ports=", 0) == 0) {
                applyPortArgument(arg.substr(8), config);
            } else if (arg.rfind("--port-range=", 0) == 0) {
                applyPortArgument(arg.substr(13), config);
            } else {
                applyPortArgument(arg, config);
            }
        }
        if (config.host.empty()) {
            config.host = DefaultHost;
        }
        return config;
    }

    json makeTextContent(const std::string& text) { return json::array({{{"type", "text"}, {"text", text}}}); }

    json makeToolErrorResult(const std::string& text) {
        return json{{"isError", true}, {"content", makeTextContent(text)}};
    }

    json makeToolTextResult(const std::string& text) {
        return json{{"isError", false}, {"content", makeTextContent(text)}};
    }

    json makeErrorResponse(const json& id, mcp::error_code code, const std::string& message) {
        return json{
            {"jsonrpc", "2.0"},
            {"id", id},
            {"error", {{"code", static_cast<int>(code)}, {"message", message}}}
        };
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
                    contentLength =
                        static_cast<size_t>(std::stoull(trim(std::string_view(firstLine).substr(colon + 1))));
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

    // 单个 MCDK 游戏 MCP 端点的会话，一个实例对应一个端口。
    class GameMcpClient {
    public:
        GameMcpClient(std::string host, int port) : host_(std::move(host)), port_(port) {}

        ~GameMcpClient() { closeSession(); }

        GameMcpClient(const GameMcpClient&)            = delete;
        GameMcpClient& operator=(const GameMcpClient&) = delete;

        [[nodiscard]] int port() const { return port_; }

        [[nodiscard]] std::string baseUrl() const { return "http://" + host_ + ":" + std::to_string(port_); }

        [[nodiscard]] std::string endpoint() const { return baseUrl() + StreamableEndpoint; }

        // 建立会话。失败时 error 为简短原因，由调用方包装成面向用户的文案。
        bool initialize(std::string& error) {
            httplib::Client client(baseUrl());
            configureClient(client, InitializationTimeoutSec);

            const json initializeRequest = {
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
                error = "cannot connect to the MCDK game MCP endpoint";
                return false;
            }
            if (result->status / 100 != 2) {
                error = "MCDK game MCP returned HTTP " + std::to_string(result->status);
                return false;
            }

            json response = json::parse(result->body, nullptr, false);
            if (response.is_discarded() || response.contains("error")) {
                error = "MCDK game MCP initialization failed";
                return false;
            }

            auto sessionHeader = result->headers.find("Mcp-Session-Id");
            if (sessionHeader == result->headers.end() || sessionHeader->second.empty()) {
                error = "MCDK game MCP did not provide a session id";
                return false;
            }

            sessionId_ = sessionHeader->second;
            connected_ = true;

            const json  initializedNotification = {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
            json        ignored;
            std::string ignoredError;
            postJson(initializedNotification, ignored, ignoredError);
            return true;
        }

        [[nodiscard]] bool healthy() { return connected_ && ping(); }

        // 主动释放后端会话，避免探测过程在 MCDK 上堆积无用会话。
        void closeSession() {
            if (!connected_ || sessionId_.empty()) {
                return;
            }
            httplib::Client client(baseUrl());
            configureClient(client, ConnectTimeoutSeconds);
            httplib::Headers headers;
            headers.emplace("Mcp-Session-Id", sessionId_);
            client.Delete(StreamableEndpoint, headers);
            connected_ = false;
            sessionId_.clear();
        }

        // 调用后端工具。连接中途失效时重连一次再重试。
        json callTool(const std::string& name, const json& arguments) {
            std::string error;
            if (!connected_ && !initialize(error)) {
                return makeToolErrorResult(notReadyMessage(error));
            }

            const auto request = [&] {
                return json{
                    {"jsonrpc", "2.0"},
                    {"id", nextId_++},
                    {"method", "tools/call"},
                    {"params", {{"name", name}, {"arguments", arguments}}}
                };
            };

            json response;
            if (!postJson(request(), response, error)) {
                connected_ = false;
                sessionId_.clear();
                if (!initialize(error) || !postJson(request(), response, error)) {
                    connected_ = false;
                    sessionId_.clear();
                    return makeToolErrorResult(notReadyMessage(error));
                }
            }

            if (response.contains("error")) {
                const auto& err     = response["error"];
                const auto  message = err.value("message", dumpJsonReplacingInvalidUtf8(response));
                if (name == mcdk::mc_profiler_mcp::ToolName && message.find("Tool not found") != std::string::npos) {
                    return json::parse(mcdk::mc_profiler_mcp::buildErrorResult(
                                           "",
                                           "BACKEND_TOOL_UNAVAILABLE",
                                           "The running MCDK backend does not expose mc_profiler. Update mcdk and "
                                           "mcdk_stdio_bridge as a matched pair.",
                                           false
                    )
                                           .dump());
                }
                return makeToolErrorResult("MCDK game MCP returned an error: " + message);
            }
            if (!response.contains("result")) {
                return makeToolErrorResult(
                    "MCDK game MCP returned an invalid response: " + dumpJsonReplacingInvalidUtf8(response)
                );
            }
            return response["result"];
        }

        [[nodiscard]] std::string notReadyMessage(const std::string& detail) const {
            return "Minecraft has not been launched through MCDK, or MCDK MCP is not enabled/configured. "
                   "Please start the game with MCDK and enable mcp_server_config first. Target endpoint: "
                 + endpoint() + ". Detail: " + detail;
        }

    private:
        bool ping() {
            json        response;
            std::string error;
            const bool  ok =
                postJson(json{{"jsonrpc", "2.0"}, {"id", nextId_++}, {"method", "ping"}}, response, error);
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
                error = "cannot connect to the MCDK game MCP endpoint";
                return false;
            }
            if (request.contains("id") && result->status / 100 != 2) {
                error = "MCDK game MCP returned HTTP " + std::to_string(result->status);
                return false;
            }
            if (!request.contains("id")) {
                response = json::object();
                return true;
            }

            response = json::parse(result->body, nullptr, false);
            if (response.is_discarded()) {
                error = "MCDK game MCP returned invalid JSON";
                return false;
            }
            return true;
        }

        void configureClient(httplib::Client& client, int readTimeoutSeconds) const {
            client.set_connection_timeout(ConnectTimeoutSeconds, 0);
            client.set_read_timeout(readTimeoutSeconds, 0);
            client.set_write_timeout(readTimeoutSeconds, 0);
        }

        std::string host_;
        int         port_ = 0;
        std::string sessionId_;
        bool        connected_ = false;
        int         nextId_    = 1;
    };

    // 负责"连到哪个游戏"：端口发现、单实例自动选中、多实例粘性选择。
    class BackendSelector {
    public:
        explicit BackendSelector(BridgeConfig config) : config_(std::move(config)) {}

        json callTool(const std::string& name, const json& arguments) {
            std::string error;
            if (!ensureClient(error)) {
                return makeToolErrorResult(error);
            }
            return client_->callTool(name, arguments);
        }

        json listInstances() {
            if (config_.lockedToSinglePort) {
                return makeToolTextResult(
                    "bridge 以固定端口 " + config_.ports.toString()
                    + " 启动，不进行实例发现。若要在多开环境下切换目标，请以端口区间启动 bridge，例如 --port "
                    + std::to_string(config_.ports.begin) + "-" + std::to_string(config_.ports.begin + 9) + "。"
                );
            }

            const auto instances = discover();
            json       data      = json::array();
            for (const auto& instance : instances) {
                data.push_back(json{
                    {"port", instance.port},
                    {"mcdk_pid", instance.mcdkPid},
                    {"minecraft_pid", instance.minecraftPid},
                    {"world_name", instance.worldName},
                    {"world_folder_name", instance.worldFolderName},
                    {"project_root", instance.projectRoot},
                    {"started_at", instance.startedAt},
                    {"selected", selectedPort_.has_value() && *selectedPort_ == instance.port},
                });
            }

            std::string text;
            if (instances.empty()) {
                text = "在端口区间 " + config_.ports.toString() + " 内没有发现正在运行的 MCDK 实例。";
            } else {
                text = "在端口区间 " + config_.ports.toString() + " 内发现 " + std::to_string(instances.size())
                     + " 个 MCDK 实例：\n";
                for (const auto& instance : instances) {
                    const bool current = selectedPort_.has_value() && *selectedPort_ == instance.port;
                    text += std::string(current ? "* " : "  ") + instance.describe() + "\n";
                }
                text += "\n使用 " + std::string(UseInstanceToolName) + " 并传入 port 可切换目标实例。";
            }

            return json{
                {"isError", false},
                {"content", makeTextContent(text)},
                {"structuredContent", json{{"port_range", config_.ports.toString()}, {"instances", std::move(data)}}}
            };
        }

        json useInstance(const json& arguments) {
            if (config_.lockedToSinglePort) {
                return makeToolErrorResult(
                    "bridge 以固定端口 " + config_.ports.toString()
                    + " 启动，无法切换实例。请改用端口区间启动 bridge 后重试。"
                );
            }

            if (!arguments.contains("port") || arguments["port"].is_null()) {
                releaseClient();
                selectedPort_.reset();
                return makeToolTextResult("已清除目标实例选择，下次调用将重新自动发现。");
            }
            if (!arguments["port"].is_number_integer()) {
                return makeToolErrorResult("参数 port 必须是整数端口号。");
            }

            const int port = arguments["port"].get<int>();
            if (!config_.ports.contains(port)) {
                return makeToolErrorResult(
                    "端口 " + std::to_string(port) + " 不在 bridge 的探测区间 " + config_.ports.toString()
                    + " 内。请调整 bridge 启动参数中的端口区间。"
                );
            }

            const auto instances = discover();
            const auto match     = std::find_if(instances.begin(), instances.end(), [port](const auto& instance) {
                return instance.port == port;
            });
            if (match == instances.end()) {
                return makeToolErrorResult(
                    "端口 " + std::to_string(port) + " 上没有发现可用的 MCDK 实例。可先调用 "
                    + std::string(ListInstancesToolName) + " 查看当前实例。"
                );
            }

            releaseClient();
            selectedPort_ = port;
            return makeToolTextResult("已切换目标实例：" + match->describe());
        }

    private:
        void releaseClient() { client_.reset(); }

        std::vector<mcdk::bridge::DiscoveredInstance> discover() {
            std::vector<mcdk::bridge::DiscoveredInstance> found;
            for (const int port : mcdk::bridge::collectCandidatePorts(config_.ports)) {
                GameMcpClient probe(config_.host, port);
                std::string   error;
                if (!probe.initialize(error)) {
                    continue;
                }

                mcdk::bridge::DiscoveredInstance instance;
                instance.port = port;

                // 旧版 MCDK 没有 mcdk_instance_info；握手成功本身已经证明这是一个可用后端，
                // 元信息取不到时只降级展示端口。
                const auto result = probe.callTool(mcdk::mcp_tool_definitions::InstanceInfoName, json::object());
                if (!result.value("isError", false) && result.contains("structuredContent")) {
                    const auto& data         = result["structuredContent"];
                    instance.mcdkPid         = data.value("mcdk_pid", 0U);
                    instance.minecraftPid    = data.value("minecraft_pid", 0U);
                    instance.worldName       = data.value("world_name", "");
                    instance.worldFolderName = data.value("world_folder_name", "");
                    instance.projectRoot     = data.value("project_root", "");
                    instance.startedAt       = data.value("started_at", "");
                    instance.hasMetadata     = true;
                }
                probe.closeSession();
                found.push_back(std::move(instance));
            }
            return found;
        }

        [[nodiscard]] std::string noInstanceMessage() const {
            return "Minecraft has not been launched through MCDK, or MCDK MCP is not enabled/configured. "
                   "Please start the game with MCDK and enable mcp_server_config first. Probed endpoints: http://"
                 + config_.host + ":" + config_.ports.toString() + StreamableEndpoint + ".";
        }

        [[nodiscard]] std::string
        multipleInstanceMessage(const std::vector<mcdk::bridge::DiscoveredInstance>& instances) const {
            std::string text = "当前有 " + std::to_string(instances.size())
                             + " 个 MCDK 实例在运行，bridge 不会替你猜测目标。请先用 "
                             + std::string(UseInstanceToolName) + " 指定端口：\n";
            for (const auto& instance : instances) {
                text += "  " + instance.describe() + "\n";
            }
            return text;
        }

        bool ensureClient(std::string& error) {
            if (client_ && client_->healthy()) {
                return true;
            }
            releaseClient();

            // 固定单值端口：保持旧版本行为，直连不发现。
            if (config_.lockedToSinglePort) {
                auto        candidate = std::make_unique<GameMcpClient>(config_.host, config_.ports.begin);
                std::string detail;
                if (!candidate->initialize(detail)) {
                    error = candidate->notReadyMessage(detail);
                    return false;
                }
                client_ = std::move(candidate);
                return true;
            }

            const auto instances = discover();
            if (instances.empty()) {
                selectedPort_.reset();
                error = noInstanceMessage();
                return false;
            }

            int target = 0;
            if (selectedPort_.has_value()) {
                const bool stillAlive = std::any_of(instances.begin(), instances.end(), [this](const auto& instance) {
                    return instance.port == *selectedPort_;
                });
                if (stillAlive) {
                    target = *selectedPort_;
                } else {
                    // 选中的实例已经退出：单实例时静默切换，多实例时交还给调用方决定。
                    const int lost = *selectedPort_;
                    selectedPort_.reset();
                    if (instances.size() > 1) {
                        error = "此前选中的实例（端口 " + std::to_string(lost) + "）已退出。"
                              + multipleInstanceMessage(instances);
                        return false;
                    }
                }
            }

            if (target == 0) {
                if (instances.size() > 1) {
                    error = multipleInstanceMessage(instances);
                    return false;
                }
                target = instances.front().port;
            }

            auto        candidate = std::make_unique<GameMcpClient>(config_.host, target);
            std::string detail;
            if (!candidate->initialize(detail)) {
                error = candidate->notReadyMessage(detail);
                return false;
            }
            client_       = std::move(candidate);
            selectedPort_ = target;
            return true;
        }

        BridgeConfig                   config_;
        std::unique_ptr<GameMcpClient> client_;
        std::optional<int>             selectedPort_;
    };

    mcp::tool buildListInstancesTool() {
        return mcp::tool_builder(ListInstancesToolName)
            .with_description(
                R"(Lists the MCDK instances the bridge can currently reach within its configured port range.

Use it in multi-instance (multi-open) testing to see which games are running and which one this bridge is
currently attached to. Each entry reports port, world name, Minecraft pid, mcdk pid and project root.

Not applicable when the bridge was started with a single fixed port.)"
            )
            .with_read_only_hint(true)
            .build();
    }

    mcp::tool buildUseInstanceTool() {
        return mcp::tool_builder(UseInstanceToolName)
            .with_description(
                R"(Selects which MCDK instance the bridge forwards subsequent tool calls to.

Call it after mcdk_instances when several games are running. The selection sticks for the rest of the
session; if the selected instance exits, the bridge falls back to automatic selection.

Parameters:
- port: Port of the target instance. Omit it to clear the selection and return to automatic discovery.)"
            )
            .with_number_param("port", "Port of the target MCDK instance", false)
            .with_read_only_hint(false)
            .build();
    }

    class BridgeServer {
    public:
        explicit BridgeServer(BridgeConfig config) : backend_(std::move(config)) {}

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

            const bool  isNotification = !message.contains("id") || message["id"].is_null();
            json        id             = isNotification ? nullptr : message["id"];
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
                // bridge 私有工具：只在 bridge 内部处理，不会转发给后端。
                tools.push_back(buildListInstancesTool().to_json());
                tools.push_back(buildUseInstanceTool().to_json());
                return makeSuccessResponse(id, json{{"tools", tools}});
            }
            if (method == "tools/call") {
                if (!params.is_object() || !params.contains("name")) {
                    return makeErrorResponse(id, mcp::error_code::invalid_params, "Missing 'name' parameter");
                }
                std::string toolName  = params.value("name", "");
                json        arguments = params.value("arguments", json::object());

                if (toolName == ListInstancesToolName) {
                    return makeSuccessResponse(id, backend_.listInstances());
                }
                if (toolName == UseInstanceToolName) {
                    return makeSuccessResponse(id, backend_.useInstance(arguments));
                }
                if (toolName == mcdk::mc_profiler_mcp::ToolName) {
                    const auto standardArguments = nlohmann::json::parse(arguments.dump());
                    auto       remoteResult      = backend_.callTool(toolName, arguments);
                    if (!remoteResult.value("isError", false)) {
                        return makeSuccessResponse(id, std::move(remoteResult));
                    }
                    if (remoteResult.contains("structuredContent")) {
                        return makeSuccessResponse(id, std::move(remoteResult));
                    }
                    if (auto localResult = mcdk::mc_profiler_mcp::tryBuildLocalResult(standardArguments)) {
                        auto converted = json::parse(localResult->dump());
                        if (converted.contains("structuredContent") && converted["structuredContent"].contains("data")) {
                            converted["structuredContent"]["data"]["runtime"] = {
                                {"status", "unavailable"},
                                {"reason",
                                 "The stdio bridge could not reach MCDK; runtime and Native DLL status are unknown."},
                            };
                        }
                        return makeSuccessResponse(id, std::move(converted));
                    }
                    return makeSuccessResponse(
                        id,
                        json::parse(mcdk::mc_profiler_mcp::buildErrorResult(
                                        standardArguments.value("op", ""),
                                        "BACKEND_UNAVAILABLE",
                                        "The stdio bridge could not reach MCDK. Runtime profiler operations are "
                                        "unavailable; the bridge never starts a local capture.",
                                        true
                        )
                                        .dump())
                    );
                }
                return makeSuccessResponse(id, backend_.callTool(toolName, arguments));
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

        BackendSelector backend_;
    };

} // namespace

int main(int argc, char** argv) {
    configureStdioAndUtf8Console();
    BridgeServer server(parseArgs(argc, argv));
    server.run();
    return 0;
}
