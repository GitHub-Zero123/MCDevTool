#include <host_bridge.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

namespace {
    constexpr std::size_t MaxTestFrameBytes = 1024 * 1024;

    bool expect(bool condition, const char* description) {
        if (!condition) {
            std::cerr << "Failed: " << description << '\n';
        }
        return condition;
    }

    bool receiveExact(SOCKET socket, void* destination, std::size_t size) {
        auto* bytes = static_cast<char*>(destination);
        std::size_t received = 0;
        while (received < size) {
            const auto remaining = std::min<std::size_t>(size - received, std::numeric_limits<int>::max());
            const int count = recv(socket, bytes + received, static_cast<int>(remaining), 0);
            if (count <= 0) {
                return false;
            }
            received += static_cast<std::size_t>(count);
        }
        return true;
    }

    bool sendExact(SOCKET socket, const void* source, std::size_t size) {
        const auto* bytes = static_cast<const char*>(source);
        std::size_t sent = 0;
        while (sent < size) {
            const auto remaining = std::min<std::size_t>(size - sent, std::numeric_limits<int>::max());
            const int count = send(socket, bytes + sent, static_cast<int>(remaining), 0);
            if (count <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(count);
        }
        return true;
    }

    bool sendFrame(SOCKET socket, const nlohmann::json& message) {
        const auto payload = message.dump();
        const auto length  = static_cast<std::uint32_t>(payload.size());
        const std::array<char, 4> header{
            static_cast<char>((length >> 24) & 0xff),
            static_cast<char>((length >> 16) & 0xff),
            static_cast<char>((length >> 8) & 0xff),
            static_cast<char>(length & 0xff),
        };
        return sendExact(socket, header.data(), header.size()) && sendExact(socket, payload.data(), payload.size());
    }

    nlohmann::json receiveFrame(SOCKET socket) {
        std::array<std::uint8_t, 4> header{};
        if (!receiveExact(socket, header.data(), header.size())) {
            return nlohmann::json();
        }
        const auto length = (static_cast<std::uint32_t>(header[0]) << 24)
                          | (static_cast<std::uint32_t>(header[1]) << 16)
                          | (static_cast<std::uint32_t>(header[2]) << 8)
                          | static_cast<std::uint32_t>(header[3]);
        if (length == 0 || length > MaxTestFrameBytes) {
            return nlohmann::json();
        }
        std::string payload(length, '\0');
        if (!receiveExact(socket, payload.data(), payload.size())) {
            return nlohmann::json();
        }
        return nlohmann::json::parse(payload, nullptr, false);
    }
}

int main() {
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        return 1;
    }

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        WSACleanup();
        return 1;
    }
    sockaddr_in address{};
    address.sin_family      = AF_INET;
    address.sin_port        = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR
        || listen(listener, 1) == SOCKET_ERROR) {
        closesocket(listener);
        WSACleanup();
        return 1;
    }
    int addressSize = sizeof(address);
    getsockname(listener, reinterpret_cast<sockaddr*>(&address), &addressSize);

    std::atomic<bool> debugEnabled = false;
    std::atomic<int>  handlerCalls = 0;
    std::atomic<bool> hostPassed   = true;

    std::thread host([&] {
        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            hostPassed.store(false);
            return;
        }
        const DWORD timeout = 3000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        const auto record = [&hostPassed](bool result) {
            if (!result) {
                hostPassed.store(false);
            }
        };

        const auto initialize = receiveFrame(client);
        record(expect(initialize.value("method", "") == "mcdk/initialize", "initialize method"));
        record(expect(
            initialize["params"].value("authToken", "") == std::string(64, 'a'),
            "initialize auth token"
        ));
        record(expect(
            !initialize["params"]["capabilities"].value("debugCapabilityEnabled", true),
            "initialize debug capability"
        ));
        record(sendFrame(
            client,
            {{"jsonrpc", "2.0"}, {"id", initialize["id"]}, {"result", {{"protocolVersion", 1}}}}
        ));

        record(sendFrame(
            client,
            {{"jsonrpc", "2.0"}, {"id", "host:disabled"}, {"method", "game/probe"}, {"params", {}}}
        ));
        const auto disabled = receiveFrame(client);
        record(expect(
            disabled["error"]["data"].value("code", "") == "DEBUG_CAPABILITY_DISABLED",
            "debug-disabled error"
        ));

        debugEnabled.store(true);
        record(sendFrame(
            client,
            {{"jsonrpc", "2.0"}, {"id", "host:not-ready"}, {"method", "game/probe"}, {"params", {}}}
        ));
        const auto notReady = receiveFrame(client);
        record(expect(
            notReady["error"]["data"].value("code", "") == "GAME_WORLD_NOT_READY",
            "world-not-ready error"
        ));
        record(expect(
            notReady["error"]["data"].value("gameIpcClientCount", 1) == 0,
            "world-not-ready state data"
        ));

        record(sendFrame(
            client,
            {{"jsonrpc", "2.0"}, {"method", "game/probe"}, {"params", {}}}
        ));
        record(sendFrame(
            client,
            {{"jsonrpc", "2.0"}, {"id", "host:ping"}, {"method", "mcdk/ping"}, {"params", {}}}
        ));
        const auto ping = receiveFrame(client);
        record(expect(
            ping.value("id", "") == "host:ping" && ping.contains("result"),
            "notification has no response"
        ));

        shutdown(client, SD_BOTH);
        closesocket(client);
    });

    mcdk::HostBridgeTask bridge({
        .configured = true,
        .enabled    = true,
        .port       = ntohs(address.sin_port),
        .token      = std::string(64, 'a'),
    });
    mcdk::RpcMethodOptions probeOptions;
    probeOptions.modes            = mcdk::RpcMode::Request | mcdk::RpcMode::Notification;
    probeOptions.execution        = mcdk::RpcExecutionPolicy::GameSerial;
    probeOptions.gameAvailability = mcdk::GameAvailability::InWorld;
    probeOptions.maxConcurrency   = 1;
    auto bindResult = bridge.registry().bindRaw(
        {.name = "game/probe"},
        probeOptions,
        [&handlerCalls](const mcdk::RpcContext&, const nlohmann::json&) -> mcdk::RpcResult {
            handlerCalls.fetch_add(1);
            return nlohmann::json::object();
        }
    );
    if (!bindResult) {
        closesocket(listener);
        host.join();
        WSACleanup();
        return 1;
    }

    bridge.setGameStateProvider([&debugEnabled] {
        return mcdk::HostBridgeGameState{
            .debugCapabilityEnabled = debugEnabled.load(),
            .gameIpcClientCount      = 0,
        };
    });
    bridge.setSessionInfo({
        .mcdkPid                = 100,
        .minecraftPid           = 200,
        .gameIpcPort            = 300,
        .debugCapabilityEnabled = false,
        .projectRoot            = std::filesystem::current_path(),
        .worldName              = "Test World",
        .worldFolderName        = "test-world",
        .worldRuntimePath       = std::filesystem::current_path() / "test-world",
    });
    bridge.start();

    host.join();
    bridge.safeExit();
    closesocket(listener);
    WSACleanup();

    return hostPassed.load() && handlerCalls.load() == 0 ? 0 : 1;
}
#else
int main() { return 0; }
#endif
