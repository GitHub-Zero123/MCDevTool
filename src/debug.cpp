#include "mcdevtool/debug.h"
#include <mcdevtool/reload.h>
#include <iostream>
#include <chrono>
#include <functional>
#include <cstring>
#include <algorithm>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

namespace MCDevTool::Debug {
#ifdef _WIN32
    namespace {
        constexpr size_t IPC_HEADER_SIZE       = 6;
        constexpr size_t IPC_MAX_PACKET_LENGTH = 16 * 1024 * 1024;

        bool readU16BE(const uint8_t* data, uint16_t& out) {
            if (!data) return false;
            out = static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | static_cast<uint16_t>(data[1]));
            return true;
        }

        bool readU32BE(const uint8_t* data, uint32_t& out) {
            if (!data) return false;
            out = (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16)
                | (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
            return true;
        }

        std::string jsonDumpNoThrow(const nlohmann::json& value) {
            try {
                return value.dump();
            } catch (...) {
                return "{}";
            }
        }
    }

    DebugIPCServer::~DebugIPCServer() { safeExit(); }

    void DebugIPCServer::start() {
        if (mSocketPtr) {
            return; // 已启动
        }
        mStopFlag = false;
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }

        SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock == INVALID_SOCKET) {
            WSACleanup();
            throw std::runtime_error("socket creation failed");
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = 0; // 系统自动分配端口
        // addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 仅本地连接

        if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
            closesocket(listenSock);
            WSACleanup();
            throw std::runtime_error("bind failed");
        }

        // 获取端口号
        int addrLen = sizeof(addr);
        getsockname(listenSock, (sockaddr*)&addr, &addrLen);
        mPort = ntohs(addr.sin_port);

        if (listen(listenSock, SOMAXCONN) == SOCKET_ERROR) {
            closesocket(listenSock);
            WSACleanup();
            throw std::runtime_error("listen failed");
        }

        mSocketPtr = reinterpret_cast<void*>(listenSock);

        // 启动客户端接入线程
        mThread = std::move(std::thread([this]() {
            SOCKET listenSock = reinterpret_cast<SOCKET>(mSocketPtr);

            // 设置监听socket为非阻塞模式，便于检查mStopFlag
            u_long nonBlocking = 1;
            ioctlsocket(listenSock, FIONBIO, &nonBlocking);

            while (!mStopFlag.load() && listenSock != INVALID_SOCKET) {
                SOCKET clientSock = accept(listenSock, nullptr, nullptr);
                if (clientSock == INVALID_SOCKET) {
                    int err = WSAGetLastError();
                    if (err == WSAEWOULDBLOCK) {
                        // 没有新连接，短暂等待后重试
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        continue;
                    }
                    // 其他错误（如socket已关闭），退出循环
                    break;
                }

                // 设置客户端socket为非阻塞
                u_long mode = 1;
                ioctlsocket(clientSock, FIONBIO, &mode);

                void* clientPtr = reinterpret_cast<void*>(clientSock);
                {
                    std::lock_guard<std::mutex> lockGuard(mClientsMutex);
                    mClients.push_back(clientPtr);
                }
                {
                    std::lock_guard<std::mutex> lockGuard(mClientThreadsMutex);
                    mClientThreads.emplace_back([this, clientPtr]() { clientReadLoop(clientPtr); });
                }
            }
        }));
    }

    void DebugIPCServer::stop() {
        mPort     = 0;
        mStopFlag = true;

        SOCKET listenSock = INVALID_SOCKET;
        if (mSocketPtr) {
            listenSock = reinterpret_cast<SOCKET>(mSocketPtr);
            mSocketPtr = nullptr;
            closesocket(listenSock);
        }

        {
            std::lock_guard<std::mutex> lockGuard(mClientsMutex);
            for (void* c : mClients) {
                closesocket(reinterpret_cast<SOCKET>(c));
            }
            mClients.clear();
        }

        std::map<uint64_t, std::shared_ptr<PendingJsonRequest>> pendingSnapshot;
        {
            std::lock_guard<std::mutex> lockGuard(mPendingJsonMutex);
            pendingSnapshot.swap(mPendingJsonRequests);
        }
        for (auto& [_, pending] : pendingSnapshot) {
            if (!pending) continue;
            {
                std::lock_guard<std::mutex> pendingLock(pending->mutex);
                pending->completed = true;
            }
            pending->cv.notify_all();
        }
    }

    bool DebugIPCServer::sendMessage(uint16_t messageType, const std::vector<uint8_t>& data) {
        return sendMessage(messageType, data.data(), data.size());
    }

    bool DebugIPCServer::sendMessage(uint16_t messageType) { return sendMessage(messageType, nullptr, 0); }

    bool DebugIPCServer::sendMessage(uint16_t messageType, std::string_view data) {
        return sendMessage(messageType, reinterpret_cast<const uint8_t*>(data.data()), data.size());
    }

    bool DebugIPCServer::sendMessage(uint16_t messageType, const uint8_t* data, size_t length) {
        if (length > UINT32_MAX || (length > 0 && !data)) {
            return false;
        }

        // 构造消息：[type(2 bytes)大端 | length(4 bytes)大端 | bytes...]
        std::vector<uint8_t> buffer(IPC_HEADER_SIZE + length);
        buffer[0]    = static_cast<uint8_t>(messageType >> 8);
        buffer[1]    = static_cast<uint8_t>(messageType & 0xFF);
        uint32_t len = static_cast<uint32_t>(length);
        buffer[2]    = (len >> 24) & 0xFF;
        buffer[3]    = (len >> 16) & 0xFF;
        buffer[4]    = (len >> 8) & 0xFF;
        buffer[5]    = len & 0xFF;
        if (length > 0) {
            memcpy(buffer.data() + IPC_HEADER_SIZE, data, length);
        }

        std::vector<void*> clientsSnapshot;
        {
            std::lock_guard<std::mutex> lockGuard(mClientsMutex);
            clientsSnapshot = mClients;
        }

        bool sentAny = false;
        for (void* clientPtr : clientsSnapshot) {
            if (sendBufferToSocket(clientPtr, buffer.data(), buffer.size())) {
                sentAny = true;
            } else {
                eraseClient(clientPtr, true);
            }
        }

        return sentAny;
    }

    bool DebugIPCServer::sendMessageToOneClient(uint16_t messageType, const uint8_t* data, size_t length) {
        if (length > UINT32_MAX || (length > 0 && !data)) {
            return false;
        }

        std::vector<uint8_t> buffer(IPC_HEADER_SIZE + length);
        buffer[0]    = static_cast<uint8_t>(messageType >> 8);
        buffer[1]    = static_cast<uint8_t>(messageType & 0xFF);
        uint32_t len = static_cast<uint32_t>(length);
        buffer[2]    = (len >> 24) & 0xFF;
        buffer[3]    = (len >> 16) & 0xFF;
        buffer[4]    = (len >> 8) & 0xFF;
        buffer[5]    = len & 0xFF;
        if (length > 0) {
            memcpy(buffer.data() + IPC_HEADER_SIZE, data, length);
        }

        void* clientPtr = nullptr;
        {
            std::lock_guard<std::mutex> lockGuard(mClientsMutex);
            if (mClients.empty()) return false;
            clientPtr = mClients.front();
        }

        if (!sendBufferToSocket(clientPtr, buffer.data(), buffer.size())) {
            eraseClient(clientPtr, true);
            return false;
        }
        return true;
    }

    bool DebugIPCServer::sendBufferToSocket(void* socketPtr, const uint8_t* data, size_t length) {
        if (!socketPtr || !data) return false;

        SOCKET clientSock = reinterpret_cast<SOCKET>(socketPtr);
        size_t sentTotal  = 0;
        auto   deadline   = std::chrono::steady_clock::now() + std::chrono::seconds(10);

        std::lock_guard<std::mutex> sendLock(mSendMutex);
        while (sentTotal < length && !mStopFlag.load()) {
            int chunkSize = static_cast<int>(std::min<size_t>(length - sentTotal, INT_MAX));
            int sent      = send(clientSock, reinterpret_cast<const char*>(data + sentTotal), chunkSize, 0);
            if (sent > 0) {
                sentTotal += static_cast<size_t>(sent);
                continue;
            }
            if (sent == 0) {
                return false;
            }

            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK) {
                return false;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return sentTotal == length;
    }

    IPCJsonResult DebugIPCServer::requestJson(std::string_view method, std::string_view paramsJson, uint32_t timeoutMs) {
        IPCJsonResult result;
        try {
            auto params = nlohmann::json::parse(paramsJson.empty() ? "{}" : std::string(paramsJson), nullptr, false);
            if (params.is_discarded()) {
                result.errorMessage = "Invalid params JSON";
                return result;
            }

            uint64_t id = mNextJsonRequestId.fetch_add(1);
            if (id == 0) {
                id = mNextJsonRequestId.fetch_add(1);
            }

            nlohmann::json req = { {"id", id}, {"method", std::string(method)}, {"params", params} };
            result             = requestJsonRaw(jsonDumpNoThrow(req), timeoutMs);
            if (result.requestId == 0) {
                result.requestId = id;
            }
            return result;
        } catch (const std::exception& e) {
            result.errorMessage = e.what();
            return result;
        } catch (...) {
            result.errorMessage = "Unknown requestJson error";
            return result;
        }
    }

    IPCJsonResult DebugIPCServer::requestJsonRaw(std::string_view requestJson, uint32_t timeoutMs) {
        IPCJsonResult result;
        try {
            if (requestJson.empty()) {
                result.errorMessage = "Empty request JSON";
                return result;
            }
            if (getClientCount() == 0) {
                result.errorMessage = "No IPC client connected";
                return result;
            }

            auto req = nlohmann::json::parse(std::string(requestJson), nullptr, false);
            if (req.is_discarded() || !req.is_object() || !req.contains("id")) {
                result.errorMessage = "Request JSON must be an object with id";
                return result;
            }

            uint64_t id = 0;
            try {
                id = req["id"].get<uint64_t>();
            } catch (...) {
                result.errorMessage = "Request id must be uint64";
                return result;
            }
            if (id == 0) {
                result.errorMessage = "Request id must not be 0";
                return result;
            }
            result.requestId = id;

            auto pending = std::make_shared<PendingJsonRequest>();
            {
                std::lock_guard<std::mutex> lockGuard(mPendingJsonMutex);
                mPendingJsonRequests[id] = pending;
            }

            bool sent = sendMessageToOneClient(
                IPC_JSON_REQUEST_TYPE,
                reinterpret_cast<const uint8_t*>(requestJson.data()),
                requestJson.size()
            );
            if (!sent) {
                std::lock_guard<std::mutex> lockGuard(mPendingJsonMutex);
                mPendingJsonRequests.erase(id);
                result.errorMessage = "Failed to send IPC JSON request";
                return result;
            }

            uint32_t safeTimeoutMs = timeoutMs == 0 ? 10000 : timeoutMs;
            std::unique_lock<std::mutex> pendingLock(pending->mutex);
            bool completed = pending->cv.wait_for(
                pendingLock,
                std::chrono::milliseconds(safeTimeoutMs),
                [&pending]() { return pending->completed; }
            );
            if (!completed) {
                pending->completed = true;
            }
            std::string responseJson = pending->responseJson;
            pendingLock.unlock();

            {
                std::lock_guard<std::mutex> lockGuard(mPendingJsonMutex);
                mPendingJsonRequests.erase(id);
            }

            if (!completed) {
                result.timeout      = true;
                result.errorMessage = "IPC JSON request timed out";
                return result;
            }

            result.success      = !responseJson.empty();
            result.responseJson = std::move(responseJson);
            if (!result.success) {
                result.errorMessage = "IPC JSON request was cancelled";
            }
            return result;
        } catch (const std::exception& e) {
            result.errorMessage = e.what();
            return result;
        } catch (...) {
            result.errorMessage = "Unknown requestJsonRaw error";
            return result;
        }
    }

    void DebugIPCServer::clientReadLoop(void* socketPtr) {
        if (!socketPtr) return;
        SOCKET clientSock = reinterpret_cast<SOCKET>(socketPtr);

        std::vector<uint8_t> buffer;
        buffer.reserve(4096);
        uint8_t temp[4096];

        while (!mStopFlag.load()) {
            int received = recv(clientSock, reinterpret_cast<char*>(temp), static_cast<int>(sizeof(temp)), 0);
            if (received > 0) {
                buffer.insert(buffer.end(), temp, temp + received);
                while (buffer.size() >= IPC_HEADER_SIZE) {
                    uint16_t typeID = 0;
                    uint32_t length = 0;
                    readU16BE(buffer.data(), typeID);
                    readU32BE(buffer.data() + 2, length);

                    if (length > IPC_MAX_PACKET_LENGTH) {
                        eraseClient(socketPtr, true);
                        return;
                    }
                    if (buffer.size() < IPC_HEADER_SIZE + static_cast<size_t>(length)) {
                        break;
                    }

                    const uint8_t* payload = buffer.data() + IPC_HEADER_SIZE;
                    if (typeID == IPC_JSON_RESPONSE_TYPE) {
                        handleJsonResponsePacket(payload, length);
                    }
                    buffer.erase(buffer.begin(), buffer.begin() + IPC_HEADER_SIZE + static_cast<size_t>(length));
                }
                continue;
            }

            if (received == 0) {
                break;
            }

            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            break;
        }

        eraseClient(socketPtr, true);
    }

    void DebugIPCServer::handleJsonResponsePacket(const uint8_t* data, size_t length) {
        if (!data || length == 0) return;
        auto response = nlohmann::json::parse(std::string(reinterpret_cast<const char*>(data), length), nullptr, false);
        if (response.is_discarded() || !response.is_object() || !response.contains("id")) {
            return;
        }

        uint64_t id = 0;
        try {
            id = response["id"].get<uint64_t>();
        } catch (...) {
            return;
        }

        std::shared_ptr<PendingJsonRequest> pending;
        {
            std::lock_guard<std::mutex> lockGuard(mPendingJsonMutex);
            auto it = mPendingJsonRequests.find(id);
            if (it == mPendingJsonRequests.end()) return;
            pending = it->second;
        }
        if (!pending) return;

        {
            std::lock_guard<std::mutex> pendingLock(pending->mutex);
            if (pending->completed) return;
            pending->responseJson = std::string(reinterpret_cast<const char*>(data), length);
            pending->completed    = true;
        }
        pending->cv.notify_all();
    }

    void DebugIPCServer::eraseClient(void* socketPtr, bool closeSocket) {
        if (!socketPtr) return;
        bool existed = false;
        {
            std::lock_guard<std::mutex> lockGuard(mClientsMutex);
            auto it = std::find(mClients.begin(), mClients.end(), socketPtr);
            if (it != mClients.end()) {
                mClients.erase(it);
                existed = true;
            }
        }
        if (existed && closeSocket) {
            closesocket(reinterpret_cast<SOCKET>(socketPtr));
        }
    }

    unsigned short DebugIPCServer::getPort() const { return mPort; }

    size_t DebugIPCServer::getClientCount() const {
        std::lock_guard<std::mutex> lockGuard(mClientsMutex);
        return mClients.size();
    }

    std::atomic<bool>* DebugIPCServer::getStopFlag() { return &mStopFlag; }

    void DebugIPCServer::join() {
        if (mThread.has_value() && mThread->joinable()) {
            mThread->join();
        }
        {
            std::lock_guard<std::mutex> lockGuard(mClientThreadsMutex);
            for (auto& thread : mClientThreads) {
                if (thread.joinable()) {
                    thread.join();
                }
            }
            mClientThreads.clear();
        }
    }

    void DebugIPCServer::safeExit() {
        stop();
        join();
    }

    void DebugIPCServer::detach() {
        if (mThread.has_value() && mThread->joinable()) {
            mThread->detach();
        }
    }

    std::thread* DebugIPCServer::getThread() { return mThread.has_value() ? &mThread.value() : nullptr; }
#endif

    std::shared_ptr<DebugIPCServer> createDebugServer() { return std::make_shared<DebugIPCServer>(); }

    HotReloadWatcherTask::HotReloadWatcherTask(int processId, const std::vector<std::filesystem::path>& modDirs)
    : mProcessId(processId),
      mModDirs(modDirs) {}

    HotReloadWatcherTask::HotReloadWatcherTask(int processId, std::vector<std::filesystem::path>&& modDirs)
    : mProcessId(processId),
      mModDirs(std::move(modDirs)) {}

    void HotReloadWatcherTask::safeExit() {
        mStopFlag = true;
        join();
        fileWatcherThread.reset();
        processWatcherThread.reset();
    }

    void HotReloadWatcherTask::start() {
        // 实现启动逻辑
        if (fileWatcherThread.has_value() || processWatcherThread.has_value()) {
            throw std::runtime_error("Watcher threads already running");
        }
        mStopFlag          = false;
        bool mNeedUpdate   = false;
        bool mIsForeground = false;
        fileWatcherThread  = MCDevTool::HotReload::watchAndReloadPyFiles(
            mModDirs,
            [this](const std::filesystem::path& path) {
                this->mNeedUpdate = true;
                this->onFileChanged(path);
                if (this->mIsForeground) {
                    this->mNeedUpdate = false;
                    this->onHotReloadTriggered();
                }
            },
            &mStopFlag
        );
        if (!fileWatcherThread.has_value()) {
            fileWatcherThread.reset();
            throw std::runtime_error("Failed to start file watcher thread");
        }
        processWatcherThread = MCDevTool::HotReload::watchProcessForegroundWindow(
            mProcessId,
            [this](bool isForeground) {
                this->mIsForeground = isForeground;
                if (isForeground && this->mNeedUpdate) {
                    this->mNeedUpdate = false;
                    this->onHotReloadTriggered();
                }
            },
            &mStopFlag
        );
        if (!processWatcherThread.has_value()) {
            processWatcherThread.reset();
            fileWatcherThread.reset();
            throw std::runtime_error("Failed to start process watcher thread");
        }
    }

    void HotReloadWatcherTask::stop() {
        // 实现停止逻辑
        if (fileWatcherThread.has_value()) {
            fileWatcherThread.reset();
        }
        if (processWatcherThread.has_value()) {
            processWatcherThread.reset();
        }
    }

    void HotReloadWatcherTask::join() {
        if (fileWatcherThread.has_value() && fileWatcherThread->joinable()) {
            fileWatcherThread->join();
        }
        if (processWatcherThread.has_value() && processWatcherThread->joinable()) {
            processWatcherThread->join();
        }
    }

    void HotReloadWatcherTask::setProcessId(int processId) { mProcessId = processId; }

    void HotReloadWatcherTask::setModDirs(const std::vector<std::filesystem::path>& modDirs) { mModDirs = modDirs; }

    void HotReloadWatcherTask::setModDirs(std::vector<std::filesystem::path>&& modDirs) {
        mModDirs = std::move(modDirs);
    }

    void HotReloadWatcherTask::onHotReloadTriggered() {}

    void HotReloadWatcherTask::onFileChanged(const std::filesystem::path& filePath) {}
} // namespace MCDevTool::Debug