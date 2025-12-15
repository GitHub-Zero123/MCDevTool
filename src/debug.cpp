#include "mcdevtool/debug.h"
#include <iostream>
// #include <uvw.hpp>

namespace MCDevTool::Debug {
    DebugIPCServer::~DebugIPCServer() { stop(); }

    void DebugIPCServer::start() {
    }

    void DebugIPCServer::stop() {
        mPort = 0;
    }

    bool DebugIPCServer::sendMessage(uint16_t messageType, const std::vector<uint8_t>& data) {
        return sendMessage(messageType, data.data(), data.size());
    }

    bool DebugIPCServer::sendMessage(uint16_t messageType, const uint8_t* data, size_t length) {
        return false;
    }
} // namespace MCDevTool::Debug