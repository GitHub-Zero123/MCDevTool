#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

namespace MCDevTool::Debug {
    class DebugIPCServer {
    public:
        DebugIPCServer() = default;
        ~DebugIPCServer();

        // 禁止复制和移动
        DebugIPCServer(const DebugIPCServer&) = delete;
        DebugIPCServer& operator=(const DebugIPCServer&) = delete;
        DebugIPCServer(DebugIPCServer&&) = delete;
        DebugIPCServer& operator=(DebugIPCServer&&) = delete;

        void start();
        void stop();

        bool sendMessage(uint16_t messageType, const std::vector<uint8_t>& data);
        bool sendMessage(uint16_t messageType, const uint8_t* data, size_t length);

        unsigned short getPort() const;
    private:
        unsigned short mPort = 0;
        void* mSocketPtr = nullptr;
    };
} // namespace MCDevTool::Debug