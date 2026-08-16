#include <mcdk/host_bridge.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <iomanip>
#include <limits>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <mcdevtool/utils.h>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

namespace mcdk {
    namespace {
        using namespace std::chrono_literals;

        constexpr std::size_t MaxFrameBytes       = 16 * 1024 * 1024;
        constexpr std::size_t MaxQueuedCalls      = 64;
        constexpr auto        ReceivePollInterval = 100ms;
        constexpr auto        HandshakeTimeout    = 3s;
        constexpr auto        HeartbeatInterval   = 10s;
        constexpr auto        HeartbeatTimeout    = 30s;

        [[nodiscard]] std::string makeUuid() {
            std::array<std::uint8_t, 16> bytes{};
            std::random_device           random;
            for (auto& byte : bytes) {
                byte = static_cast<std::uint8_t>(random());
            }
            bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0f) | 0x40);
            bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3f) | 0x80);

            std::ostringstream output;
            output << std::hex << std::setfill('0');
            for (std::size_t index = 0; index < bytes.size(); ++index) {
                if (index == 4 || index == 6 || index == 8 || index == 10) {
                    output << '-';
                }
                output << std::setw(2) << static_cast<unsigned int>(bytes[index]);
            }
            return output.str();
        }

        [[nodiscard]] std::string utcNow() {
            const auto now          = std::chrono::system_clock::now();
            const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch())
                                    % 1000;
            const auto time = std::chrono::system_clock::to_time_t(now);
            std::tm    utc{};
#ifdef _WIN32
            gmtime_s(&utc, &time);
#else
            gmtime_r(&time, &utc);
#endif
            std::ostringstream output;
            output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0')
                   << milliseconds.count() << 'Z';
            return output.str();
        }

        [[nodiscard]] std::vector<std::uint8_t> encodeFrame(const nlohmann::json& message) {
            const auto payload = message.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            if (payload.empty() || payload.size() > MaxFrameBytes) {
                return {};
            }
            const auto length = static_cast<std::uint32_t>(payload.size());
            std::vector<std::uint8_t> frame(4 + payload.size());
            frame[0] = static_cast<std::uint8_t>((length >> 24) & 0xff);
            frame[1] = static_cast<std::uint8_t>((length >> 16) & 0xff);
            frame[2] = static_cast<std::uint8_t>((length >> 8) & 0xff);
            frame[3] = static_cast<std::uint8_t>(length & 0xff);
            std::copy(payload.begin(), payload.end(), frame.begin() + 4);
            return frame;
        }

        [[nodiscard]] std::optional<std::uint32_t> readFrameLength(const std::uint8_t* data) {
            if (data == nullptr) {
                return std::nullopt;
            }
            return (static_cast<std::uint32_t>(data[0]) << 24) | (static_cast<std::uint32_t>(data[1]) << 16)
                 | (static_cast<std::uint32_t>(data[2]) << 8) | static_cast<std::uint32_t>(data[3]);
        }

        [[nodiscard]] nlohmann::json makeErrorResponse(const nlohmann::json& id, const RpcError& error) {
            return {
                {"jsonrpc", "2.0"},
                {"id", id},
                {"error", {{"code", error.code}, {"message", error.message}, {"data", error.data}}},
            };
        }

        [[nodiscard]] nlohmann::json makeSuccessResponse(const nlohmann::json& id, nlohmann::json result) {
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
        }

        [[nodiscard]] bool validRequestId(const nlohmann::json& id) {
            if (id.is_string()) {
                return !id.get_ref<const std::string&>().empty();
            }
            if (id.is_number_unsigned()) {
                return id.get<std::uint64_t>()
                    <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
            }
            return id.is_number_integer();
        }

        [[nodiscard]] const char* stateName(const HostBridgeGameState& state, bool minecraftExited, bool wasInWorld) {
            if (minecraftExited) {
                return "exited";
            }
            if (!state.debugCapabilityEnabled) {
                return "game_unavailable";
            }
            if (state.gameIpcClientCount > 0) {
                return "game_ready";
            }
            return wasInWorld ? "game_unavailable" : "process_started";
        }
    } // namespace

    class HostBridgeTask::Impl {
    public:
        explicit Impl(HostBridgeConfig bridgeConfig) : config(std::move(bridgeConfig)) { bindCoreMethods(); }

        ~Impl() { safeExit(); }

        struct WorkItem {
            std::uint64_t                       generation = 0;
            const RpcMethodEntry*               entry      = nullptr;
            std::string                         method;
            nlohmann::json                      id         = nullptr;
            nlohmann::json                      params     = nlohmann::json::object();
            bool                                notification = false;
            std::shared_ptr<std::atomic<bool>> cancelled;
        };

        struct OutboundItem {
            std::uint64_t  generation = 0;
            nlohmann::json message;
        };

        void bindCoreMethods() {
            auto mustBind = [this](std::expected<void, RpcBindError> result) {
                if (!result) {
                    throw std::runtime_error("Failed to register Host Bridge core RPC method");
                }
            };

            RpcMethodOptions inlineOptions;
            inlineOptions.execution = RpcExecutionPolicy::Inline;

            mustBind(registry.bindRaw(
                {.name = "mcdk/ping", .paramsSchema = {{"type", "object"}}, .resultSchema = {{"type", "object"}}},
                inlineOptions,
                [](const RpcContext&, const nlohmann::json&) -> RpcResult {
                    return nlohmann::json{{"receivedAt", utcNow()}};
                }
            ));

            mustBind(registry.bindRaw(
                {.name = "mcdk/session/get", .paramsSchema = {{"type", "object"}}, .resultSchema = {{"type", "object"}}},
                inlineOptions,
                [this](const RpcContext&, const nlohmann::json&) -> RpcResult { return buildSessionSnapshot(); }
            ));

            mustBind(registry.bindRaw(
                {.name = "mcdk/methods/list", .paramsSchema = {{"type", "object"}}, .resultSchema = {{"type", "object"}}},
                inlineOptions,
                [this](const RpcContext&, const nlohmann::json&) -> RpcResult { return registry.describeMethods(); }
            ));
        }

        void output(ConsoleColor color, const std::string& message) const {
            ConsoleOutputCallback callback;
            {
                std::lock_guard lock(stateMutex);
                callback = outputCallback;
            }
            if (callback) {
                callback(message, color);
            }
        }

        void setOutputCallback(ConsoleOutputCallback callback) {
            std::lock_guard lock(stateMutex);
            outputCallback = std::move(callback);
        }

        void setSessionInfo(HostBridgeSessionInfo value) {
            std::lock_guard lock(stateMutex);
            if (running.load(std::memory_order_acquire)) {
                throw std::logic_error("HostBridgeTask session cannot change while running");
            }
            if (value.sessionId.empty()) {
                value.sessionId = makeUuid();
            }
            if (value.startedAt.empty()) {
                value.startedAt = utcNow();
            }
            session = std::move(value);
        }

        void setGameStateProvider(GameStateProvider provider) {
            std::lock_guard lock(stateMutex);
            gameStateProvider = std::move(provider);
        }

        [[nodiscard]] HostBridgeGameState gameState() const {
            GameStateProvider provider;
            HostBridgeSessionInfo sessionCopy;
            {
                std::lock_guard lock(stateMutex);
                provider    = gameStateProvider;
                sessionCopy = session;
            }
            if (provider) {
                try {
                    return provider();
                } catch (...) {
                }
            }
            return {.debugCapabilityEnabled = sessionCopy.debugCapabilityEnabled, .gameIpcClientCount = 0};
        }

        [[nodiscard]] nlohmann::json buildSessionSnapshot() const {
            HostBridgeSessionInfo sessionCopy;
            bool                  hasExited = false;
            std::optional<std::uint32_t> exitCode;
            {
                std::lock_guard lock(stateMutex);
                sessionCopy = session;
                hasExited   = minecraftExited;
                exitCode    = minecraftExitCode;
            }
            const auto game = gameState();
            const bool inWorld = game.debugCapabilityEnabled && game.gameIpcClientCount > 0;
            const auto currentState = stateName(game, hasExited, wasInWorld.load(std::memory_order_relaxed));

            nlohmann::json sourcePath = nullptr;
            if (sessionCopy.worldSourcePath) {
                sourcePath = MCDevTool::Utils::pathToGenericUtf8(*sessionCopy.worldSourcePath);
            }
            nlohmann::json snapshot = {
                {"session",
                 {{"id", sessionCopy.sessionId},
                  {"startedAt", sessionCopy.startedAt},
                  {"state", currentState},
                  {"stateSequence", stateSequence.load(std::memory_order_relaxed)}}},
                {"mcdk", {{"pid", sessionCopy.mcdkPid}, {"version", "0.1.0"}}},
                {"minecraft", {{"pid", sessionCopy.minecraftPid}, {"exitCode", exitCode ? nlohmann::json(*exitCode) : nlohmann::json(nullptr)}}},
                {"gameIpc",
                 {{"host", "127.0.0.1"},
                  {"port", sessionCopy.gameIpcPort},
                  {"connected", inWorld},
                  {"clientCount", game.gameIpcClientCount}}},
                {"project", {{"root", MCDevTool::Utils::pathToGenericUtf8(sessionCopy.projectRoot)}}},
                {"world",
                 {{"name", sessionCopy.worldName},
                  {"folderName", sessionCopy.worldFolderName},
                  {"runtimePath", MCDevTool::Utils::pathToGenericUtf8(sessionCopy.worldRuntimePath)},
                  {"sourcePath", std::move(sourcePath)}}},
                {"capabilities",
                 {{"methodDiscovery", true},
                  {"notifications", true},
                  {"cancellation", false},
                  {"debugCapabilityEnabled", game.debugCapabilityEnabled}}},
                {"limits", {{"maxFrameBytes", MaxFrameBytes}, {"maxInFlightRequests", MaxQueuedCalls}}},
            };
            return snapshot;
        }

        [[nodiscard]] RpcError availabilityError(GameAvailability availability) const {
            const auto game = gameState();
            HostBridgeSessionInfo sessionCopy;
            bool hasExited;
            {
                std::lock_guard lock(stateMutex);
                sessionCopy = session;
                hasExited   = minecraftExited;
            }
            const bool inWorld = game.debugCapabilityEnabled && game.gameIpcClientCount > 0 && !hasExited;
            const auto currentState = stateName(game, hasExited, wasInWorld.load(std::memory_order_relaxed));
            nlohmann::json data = {
                {"retryable", game.debugCapabilityEnabled},
                {"sessionId", sessionCopy.sessionId},
                {"state", currentState},
                {"minecraftPid", sessionCopy.minecraftPid},
                {"debugCapabilityEnabled", game.debugCapabilityEnabled},
                {"inWorld", inWorld},
                {"gameIpcClientCount", game.gameIpcClientCount},
            };
            if (!game.debugCapabilityEnabled && availability != GameAvailability::None) {
                data["code"] = "DEBUG_CAPABILITY_DISABLED";
                return {.code = -32010, .message = "Debug capability is not enabled", .data = std::move(data)};
            }
            data["code"] = "GAME_WORLD_NOT_READY";
            return {.code = -32011, .message = "Minecraft has not entered a world", .data = std::move(data)};
        }

        [[nodiscard]] std::optional<RpcError> checkAvailability(GameAvailability availability) const {
            if (availability == GameAvailability::None) {
                return std::nullopt;
            }
            const auto game = gameState();
            if (!game.debugCapabilityEnabled) {
                return availabilityError(availability);
            }
            if (availability == GameAvailability::InWorld
                && (game.gameIpcClientCount == 0 || minecraftExitedValue())) {
                return availabilityError(availability);
            }
            return std::nullopt;
        }

        [[nodiscard]] bool minecraftExitedValue() const {
            std::lock_guard lock(stateMutex);
            return minecraftExited;
        }

        void start() {
            if (!config.errorMessage.empty()) {
                output(ConsoleColor::Yellow, "[HostBridge] Disabled: " + config.errorMessage);
                return;
            }
            if (!config.enabled || running.exchange(true, std::memory_order_acq_rel)) {
                return;
            }
            {
                std::lock_guard lock(stateMutex);
                if (session.minecraftPid == 0 || session.mcdkPid == 0 || session.sessionId.empty()) {
                    running.store(false, std::memory_order_release);
                    throw std::logic_error("HostBridgeTask requires complete session info before start");
                }
            }
            registry.seal();
            stopFlag.store(false, std::memory_order_release);
            workerThread  = std::thread([this] { workerLoop(); });
            networkThread = std::thread([this] { networkLoop(); });
        }

        void stop() {
            stopFlag.store(true, std::memory_order_release);
            workCv.notify_all();
            retryCv.notify_all();
#ifdef _WIN32
            std::lock_guard lock(socketMutex);
            if (activeSocket != INVALID_SOCKET) {
                shutdown(activeSocket, SD_BOTH);
            }
#endif
        }

        void join() {
            if (networkThread.joinable()) {
                networkThread.join();
            }
            if (workerThread.joinable()) {
                workerThread.join();
            }
            running.store(false, std::memory_order_release);
        }

        void safeExit() {
            stop();
            join();
        }

        void notifyMinecraftExited(std::uint32_t code) {
            {
                std::lock_guard lock(stateMutex);
                minecraftExited   = true;
                minecraftExitCode = code;
            }
            queueStateNotification("exiting", std::nullopt);
            queueStateNotification("exited", code);

            const auto deadline = std::chrono::steady_clock::now() + 500ms;
            std::unique_lock lock(outboundMutex);
            outboundCv.wait_until(lock, deadline, [this] { return outbound.empty() || !connected.load(); });
        }

        void queueStateNotification(std::string_view state, std::optional<std::uint32_t> exitCode) {
            const auto sequence = stateSequence.fetch_add(1, std::memory_order_relaxed) + 1;
            const auto game     = gameState();
            HostBridgeSessionInfo sessionCopy;
            {
                std::lock_guard lock(stateMutex);
                sessionCopy = session;
            }
            queueOutbound(
                activeGeneration.load(std::memory_order_relaxed),
                {
                    {"jsonrpc", "2.0"},
                    {"method", "mcdk/session/stateChanged"},
                    {"params",
                     {{"sessionId", sessionCopy.sessionId},
                      {"sequence", sequence},
                      {"state", state},
                      {"timestamp", utcNow()},
                      {"gameIpcConnected", game.gameIpcClientCount > 0},
                      {"reason", nullptr},
                      {"minecraftExitCode", exitCode ? nlohmann::json(*exitCode) : nlohmann::json(nullptr)}}},
                }
            );
        }

        void queueOutbound(std::uint64_t generation, nlohmann::json message) {
            if (generation == 0) {
                return;
            }
            {
                std::lock_guard lock(outboundMutex);
                outbound.push_back({generation, std::move(message)});
            }
            outboundCv.notify_all();
        }

        void workerLoop() {
            while (true) {
                WorkItem item;
                {
                    std::unique_lock lock(workMutex);
                    workCv.wait(lock, [this] { return stopFlag.load(std::memory_order_acquire) || !work.empty(); });
                    if (stopFlag.load(std::memory_order_acquire)) {
                        return;
                    }
                    item = std::move(work.front());
                    work.pop_front();
                }

                if (item.generation != activeGeneration.load(std::memory_order_acquire)) {
                    item.cancelled->store(true, std::memory_order_relaxed);
                    continue;
                }

                RpcResult result = std::unexpected(RpcError{});
                if (const auto availability = checkAvailability(item.entry->options.gameAvailability)) {
                    result = std::unexpected(*availability);
                } else {
                    try {
                        HostBridgeSessionInfo sessionCopy;
                        {
                            std::lock_guard lock(stateMutex);
                            sessionCopy = session;
                        }
                        RpcContext context{
                            .method       = item.method,
                            .id           = item.id,
                            .notification = item.notification,
                            .deadline     = std::chrono::steady_clock::now() + item.entry->options.timeout,
                            .sessionId    = sessionCopy.sessionId,
                            .cancelled    = item.cancelled,
                        };
                        result = item.entry->handler(context, item.params);
                        if (!result && result.error().code == -32011) {
                            result = std::unexpected(availabilityError(GameAvailability::InWorld));
                        }
                    } catch (const std::exception& error) {
                        result = std::unexpected(RpcError{
                            .code    = -32603,
                            .message = "Internal error",
                            .data    = {{"code", "INTERNAL_ERROR"}, {"detail", error.what()}},
                        });
                    } catch (...) {
                        result = std::unexpected(RpcError{});
                    }
                }

                if (!item.notification) {
                    queueOutbound(
                        item.generation,
                        result ? makeSuccessResponse(item.id, std::move(*result))
                               : makeErrorResponse(item.id, result.error())
                    );
                } else if (!result) {
                    output(
                        ConsoleColor::Yellow,
                        "[HostBridge] Notification rejected: " + item.method + " ("
                            + result.error().data.value("code", "INTERNAL_ERROR") + ")"
                    );
                }
            }
        }

        void dispatchMessage(std::uint64_t generation, const nlohmann::json& message) {
            if (!message.is_object() || message.value("jsonrpc", "") != "2.0" || !message.contains("method")
                || !message["method"].is_string()) {
                if (message.is_object() && message.contains("id")) {
                    queueOutbound(
                        generation,
                        makeErrorResponse(
                            message["id"],
                            {.code = -32600, .message = "Invalid request", .data = {{"code", "INVALID_REQUEST"}}}
                        )
                    );
                }
                return;
            }

            const bool notification = !message.contains("id");
            if (!notification && !validRequestId(message["id"])) {
                queueOutbound(
                    generation,
                    makeErrorResponse(
                        nullptr,
                        {.code = -32600, .message = "Invalid request id", .data = {{"code", "INVALID_REQUEST"}}}
                    )
                );
                return;
            }

            const auto& method = message["method"].get_ref<const std::string&>();
            const auto* entry  = registry.find(method);
            if (entry == nullptr) {
                if (!notification) {
                    queueOutbound(
                        generation,
                        makeErrorResponse(
                            message["id"],
                            {.code = -32601,
                             .message = "Method not found",
                             .data = {{"code", "METHOD_NOT_FOUND"}, {"method", method}}}
                        )
                    );
                }
                return;
            }

            const auto mode = notification ? RpcMode::Notification : RpcMode::Request;
            if (!rpcModesContain(entry->options.modes, mode)) {
                if (!notification) {
                    queueOutbound(
                        generation,
                        makeErrorResponse(
                            message["id"],
                            {.code = -32005,
                             .message = "RPC mode is not allowed",
                             .data = {{"code", "MODE_NOT_ALLOWED"}, {"method", method}}}
                        )
                    );
                }
                return;
            }

            WorkItem item{
                .generation   = generation,
                .entry        = entry,
                .method       = method,
                .id           = notification ? nlohmann::json(nullptr) : message["id"],
                .params       = message.value("params", nlohmann::json::object()),
                .notification = notification,
                .cancelled    = std::make_shared<std::atomic<bool>>(false),
            };
            {
                std::lock_guard lock(workMutex);
                if (work.size() >= MaxQueuedCalls) {
                    if (!notification) {
                        queueOutbound(
                            generation,
                            makeErrorResponse(
                                item.id,
                                {.code = -32004,
                                 .message = "Host Bridge is busy",
                                 .data = {{"code", "SERVER_BUSY"}}}
                            )
                        );
                    }
                    return;
                }
                work.push_back(std::move(item));
            }
            workCv.notify_one();
        }

#ifdef _WIN32
        [[nodiscard]] bool sendMessage(SOCKET socket, const nlohmann::json& message) {
            auto frame = encodeFrame(message);
            if (frame.empty()) {
                return false;
            }
            std::size_t sent = 0;
            while (sent < frame.size() && !stopFlag.load(std::memory_order_acquire)) {
                const auto remaining = std::min<std::size_t>(frame.size() - sent, std::numeric_limits<int>::max());
                const int count = send(
                    socket,
                    reinterpret_cast<const char*>(frame.data() + sent),
                    static_cast<int>(remaining),
                    0
                );
                if (count <= 0) {
                    return false;
                }
                sent += static_cast<std::size_t>(count);
            }
            return sent == frame.size();
        }

        [[nodiscard]] bool receiveFrames(
            SOCKET socket,
            std::vector<std::uint8_t>& buffer,
            std::vector<nlohmann::json>& messages
        ) {
            std::array<std::uint8_t, 8192> chunk{};
            const int count = recv(socket, reinterpret_cast<char*>(chunk.data()), static_cast<int>(chunk.size()), 0);
            if (count == 0) {
                return false;
            }
            if (count < 0) {
                const int error = WSAGetLastError();
                return error == WSAETIMEDOUT || error == WSAEWOULDBLOCK;
            }
            buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + count);

            std::size_t consumed = 0;
            while (buffer.size() - consumed >= 4) {
                const auto length = readFrameLength(buffer.data() + consumed);
                if (!length || *length == 0 || *length > MaxFrameBytes) {
                    return false;
                }
                const auto frameSize = 4 + static_cast<std::size_t>(*length);
                if (buffer.size() - consumed < frameSize) {
                    break;
                }
                const auto* begin = reinterpret_cast<const char*>(buffer.data() + consumed + 4);
                auto message = nlohmann::json::parse(begin, begin + *length, nullptr, false, true);
                if (message.is_discarded() || !message.is_object()) {
                    return false;
                }
                messages.push_back(std::move(message));
                consumed += frameSize;
            }
            if (consumed != 0) {
                buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(consumed));
            }
            return true;
        }

        [[nodiscard]] SOCKET connectSocket() const {
            SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (socket == INVALID_SOCKET) {
                return INVALID_SOCKET;
            }
            sockaddr_in address{};
            address.sin_family      = AF_INET;
            address.sin_port        = htons(config.port);
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            if (::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
                closesocket(socket);
                return INVALID_SOCKET;
            }
            const DWORD receiveTimeout = static_cast<DWORD>(ReceivePollInterval.count());
            const DWORD sendTimeout    = 1000;
            setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&receiveTimeout), sizeof(receiveTimeout));
            setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&sendTimeout), sizeof(sendTimeout));
            const BOOL noDelay = TRUE;
            setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
            return socket;
        }

        [[nodiscard]] nlohmann::json initializeRequest(std::uint64_t generation) const {
            auto snapshot = buildSessionSnapshot();
            snapshot["session"]["connectionGeneration"] = generation;
            snapshot["protocol"]  = {{"minVersion", 1}, {"maxVersion", 1}};
            snapshot["authToken"] = config.token;
            return {
                {"jsonrpc", "2.0"},
                {"id", "mcdk:init:" + std::to_string(generation)},
                {"method", "mcdk/initialize"},
                {"params", std::move(snapshot)},
            };
        }

        enum class HandshakeResult {
            Success,
            Retry,
            PermanentFailure,
        };

        [[nodiscard]] HandshakeResult handshake(
            SOCKET socket,
            std::uint64_t generation,
            std::vector<std::uint8_t>& receiveBuffer
        ) {
            const auto request = initializeRequest(generation);
            const auto expectedId = request["id"];
            if (!sendMessage(socket, request)) {
                return HandshakeResult::Retry;
            }

            const auto deadline = std::chrono::steady_clock::now() + HandshakeTimeout;
            while (!stopFlag.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
                std::vector<nlohmann::json> messages;
                if (!receiveFrames(socket, receiveBuffer, messages)) {
                    return HandshakeResult::Retry;
                }
                for (auto& message : messages) {
                    if (!message.contains("id") || message["id"] != expectedId) {
                        continue;
                    }
                    if (message.contains("error")) {
                        const auto code = message["error"].value("code", 0);
                        if (code == -32001 || code == -32002) {
                            output(ConsoleColor::Red, "[HostBridge] Handshake rejected by Host");
                            return HandshakeResult::PermanentFailure;
                        }
                        return HandshakeResult::Retry;
                    }
                    if (!message.contains("result") || message["result"].value("protocolVersion", 0) != 1) {
                        return HandshakeResult::PermanentFailure;
                    }
                    return HandshakeResult::Success;
                }
            }
            return HandshakeResult::Retry;
        }

        void flushOutbound(SOCKET socket, std::uint64_t generation, std::chrono::steady_clock::time_point& activity) {
            while (true) {
                OutboundItem item;
                {
                    std::lock_guard lock(outboundMutex);
                    while (!outbound.empty() && outbound.front().generation != generation) {
                        outbound.pop_front();
                    }
                    if (outbound.empty()) {
                        outboundCv.notify_all();
                        return;
                    }
                    item = std::move(outbound.front());
                    outbound.pop_front();
                }
                if (!sendMessage(socket, item.message)) {
                    throw std::runtime_error("Host Bridge send failed");
                }
                activity = std::chrono::steady_clock::now();
                outboundCv.notify_all();
            }
        }

        [[nodiscard]] bool activeLoop(SOCKET socket, std::uint64_t generation, std::vector<std::uint8_t>& receiveBuffer) {
            auto lastActivity  = std::chrono::steady_clock::now();
            auto lastReceive   = lastActivity;
            auto heartbeatId   = std::string{};
            const auto initialGameState = gameState();
            bool lastInWorld = initialGameState.debugCapabilityEnabled && initialGameState.gameIpcClientCount > 0;
            wasInWorld.store(lastInWorld, std::memory_order_relaxed);

            while (!stopFlag.load(std::memory_order_acquire)) {
                try {
                    flushOutbound(socket, generation, lastActivity);
                } catch (...) {
                    return false;
                }

                std::vector<nlohmann::json> messages;
                if (!receiveFrames(socket, receiveBuffer, messages)) {
                    return false;
                }
                if (!messages.empty()) {
                    lastReceive  = std::chrono::steady_clock::now();
                    lastActivity = lastReceive;
                }
                for (const auto& message : messages) {
                    if (message.contains("method")) {
                        dispatchMessage(generation, message);
                    } else if (message.contains("id") && message["id"].is_string()
                               && message["id"].get_ref<const std::string&>() == heartbeatId) {
                        heartbeatId.clear();
                    }
                }

                const auto currentGameState = gameState();
                const bool inWorld = currentGameState.debugCapabilityEnabled
                                  && currentGameState.gameIpcClientCount > 0;
                if (inWorld != lastInWorld) {
                    wasInWorld.store(wasInWorld.load(std::memory_order_relaxed) || inWorld, std::memory_order_relaxed);
                    queueStateNotification(inWorld ? "game_ready" : "game_unavailable", std::nullopt);
                    lastInWorld = inWorld;
                }

                const auto now = std::chrono::steady_clock::now();
                if (heartbeatId.empty() && now - lastActivity >= HeartbeatInterval) {
                    const auto id = "mcdk:ping:" + std::to_string(nextRequestId.fetch_add(1));
                    if (!sendMessage(
                            socket,
                            {{"jsonrpc", "2.0"}, {"id", id}, {"method", "mcdk/ping"}, {"params", nlohmann::json::object()}}
                        )) {
                        return false;
                    }
                    heartbeatId = id;
                    lastActivity = now;
                }
                if (now - lastReceive >= HeartbeatTimeout) {
                    return false;
                }
            }
            return true;
        }

        void networkLoop() {
            WSADATA winsock{};
            if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
                output(ConsoleColor::Red, "[HostBridge] WSAStartup failed");
                return;
            }

            constexpr std::array<std::chrono::milliseconds, 6> retryDelays{
                100ms,
                200ms,
                500ms,
                1000ms,
                2000ms,
                5000ms,
            };
            std::size_t retryIndex = 0;
            bool announcedWaiting = false;
            while (!stopFlag.load(std::memory_order_acquire)) {
                SOCKET socket = connectSocket();
                if (socket == INVALID_SOCKET) {
                    if (!announcedWaiting) {
                        output(
                            ConsoleColor::Yellow,
                            "[HostBridge] Waiting for Host on 127.0.0.1:" + std::to_string(config.port)
                        );
                        announcedWaiting = true;
                    }
                    waitRetry(retryDelays[std::min(retryIndex++, retryDelays.size() - 1)]);
                    continue;
                }
                {
                    std::lock_guard lock(socketMutex);
                    activeSocket = socket;
                }

                const auto generation = connectionGeneration.fetch_add(1) + 1;
                std::vector<std::uint8_t> receiveBuffer;
                const auto handshakeResult = handshake(socket, generation, receiveBuffer);
                if (handshakeResult == HandshakeResult::Success) {
                    activeGeneration.store(generation, std::memory_order_release);
                    connected.store(true, std::memory_order_release);
                    // 静默处理
                    // output(ConsoleColor::Green, "[HostBridge] Connected to Host");
                    announcedWaiting = false;
                    retryIndex       = 0;
                    (void)activeLoop(socket, generation, receiveBuffer);
                }

                connected.store(false, std::memory_order_release);
                activeGeneration.store(0, std::memory_order_release);
                {
                    std::lock_guard lock(socketMutex);
                    if (activeSocket == socket) {
                        activeSocket = INVALID_SOCKET;
                    }
                }
                shutdown(socket, SD_BOTH);
                closesocket(socket);

                if (handshakeResult == HandshakeResult::PermanentFailure) {
                    break;
                }
                if (!stopFlag.load(std::memory_order_acquire)) {
                    waitRetry(retryDelays[std::min(retryIndex++, retryDelays.size() - 1)]);
                }
            }
            WSACleanup();
        }

        template <class Duration>
        void waitRetry(Duration duration) {
            std::unique_lock lock(retryMutex);
            retryCv.wait_for(lock, duration, [this] { return stopFlag.load(std::memory_order_acquire); });
        }
#else
        void networkLoop() {
            output(ConsoleColor::Yellow, "[HostBridge] This transport is only available on Windows");
        }
#endif

        HostBridgeConfig config;
        RpcRegistry      registry;

        mutable std::mutex    stateMutex;
        ConsoleOutputCallback outputCallback;
        HostBridgeSessionInfo session;
        GameStateProvider     gameStateProvider;
        bool                  minecraftExited = false;
        std::optional<std::uint32_t> minecraftExitCode;

        std::atomic<bool>          stopFlag = false;
        std::atomic<bool>          running  = false;
        std::atomic<bool>          connected = false;
        std::atomic<bool>          wasInWorld = false;
        std::atomic<std::uint64_t> connectionGeneration = 0;
        std::atomic<std::uint64_t> activeGeneration = 0;
        std::atomic<std::uint64_t> nextRequestId = 1;
        std::atomic<std::uint64_t> stateSequence = 1;

        std::thread networkThread;
        std::thread workerThread;

        std::mutex              workMutex;
        std::condition_variable workCv;
        std::deque<WorkItem>    work;

        std::mutex               outboundMutex;
        std::condition_variable  outboundCv;
        std::deque<OutboundItem> outbound;

        std::mutex              retryMutex;
        std::condition_variable retryCv;

#ifdef _WIN32
        std::mutex socketMutex;
        SOCKET     activeSocket = INVALID_SOCKET;
#endif
    };

    HostBridgeTask::HostBridgeTask(HostBridgeConfig config) : mImpl(std::make_unique<Impl>(std::move(config))) {}

    HostBridgeTask::~HostBridgeTask() = default;

    void HostBridgeTask::setOutputCallback(ConsoleOutputCallback callback) {
        mImpl->setOutputCallback(std::move(callback));
    }

    void HostBridgeTask::setSessionInfo(HostBridgeSessionInfo sessionInfo) {
        mImpl->setSessionInfo(std::move(sessionInfo));
    }

    void HostBridgeTask::setGameStateProvider(GameStateProvider provider) {
        mImpl->setGameStateProvider(std::move(provider));
    }

    RpcRegistry& HostBridgeTask::registry() { return mImpl->registry; }

    bool HostBridgeTask::enabled() const noexcept { return mImpl->config.enabled; }

    bool HostBridgeTask::connected() const noexcept { return mImpl->connected.load(std::memory_order_acquire); }

    void HostBridgeTask::start() { mImpl->start(); }

    void HostBridgeTask::notifyMinecraftExited(std::uint32_t exitCode) { mImpl->notifyMinecraftExited(exitCode); }

    void HostBridgeTask::stop() { mImpl->stop(); }

    void HostBridgeTask::join() { mImpl->join(); }

    void HostBridgeTask::safeExit() { mImpl->safeExit(); }

} // namespace mcdk
