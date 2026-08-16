#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include <mcdk/console.hpp>
#include <mcdk/env.hpp>
#include <mcdk/rpc_registry.hpp>

namespace mcdk {

    struct HostBridgeSessionInfo {
        std::string           sessionId;
        std::string           startedAt;
        std::uint32_t         mcdkPid      = 0;
        std::uint32_t         minecraftPid = 0;
        std::uint16_t         gameIpcPort  = 0;
        bool                  debugCapabilityEnabled = false;
        std::filesystem::path projectRoot;
        std::string           worldName;
        std::string           worldFolderName;
        std::filesystem::path worldRuntimePath;
        std::optional<std::filesystem::path> worldSourcePath;
    };

    struct HostBridgeGameState {
        bool        debugCapabilityEnabled = false;
        std::size_t gameIpcClientCount      = 0;
    };

    class HostBridgeTask {
    public:
        using GameStateProvider = std::function<HostBridgeGameState()>;

        explicit HostBridgeTask(HostBridgeConfig config = {});
        ~HostBridgeTask();

        HostBridgeTask(const HostBridgeTask&)            = delete;
        HostBridgeTask& operator=(const HostBridgeTask&) = delete;
        HostBridgeTask(HostBridgeTask&&)                 = delete;
        HostBridgeTask& operator=(HostBridgeTask&&)      = delete;

        void setOutputCallback(ConsoleOutputCallback callback);
        void setSessionInfo(HostBridgeSessionInfo sessionInfo);
        void setGameStateProvider(GameStateProvider provider);

        [[nodiscard]] RpcRegistry& registry();
        [[nodiscard]] bool         enabled() const noexcept;
        [[nodiscard]] bool         connected() const noexcept;

        void start();
        void notifyMinecraftExited(std::uint32_t exitCode);
        void stop();
        void join();
        void safeExit();

    private:
        class Impl;
        std::unique_ptr<Impl> mImpl;
    };

} // namespace mcdk
