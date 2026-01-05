#include "mcdevtool/debug.h"
#include <mcdevtool/reload.h>
#include <iostream>
#include <chrono>
#include <functional>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

namespace MCDevTool::Debug {
#ifdef _WIN32
    DebugIPCServer::~DebugIPCServer() { stop(); }

    void DebugIPCServer::start() {
        if (mSocketPtr) {
            return; // 已启动
        }
        mStopFlag = false;
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }

        SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock == INVALID_SOCKET) {
            WSACleanup();
            throw std::runtime_error("socket creation failed");
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0; // 系统自动分配端口
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

                std::lock_guard<std::mutex> lockGuard(mClientsMutex);
                mClients.push_back(reinterpret_cast<void*>(clientSock));
            }
        }));
    }

    void DebugIPCServer::stop() {
        mPort = 0;
        mStopFlag = true;
        if (!mSocketPtr) {
            return;
        }
        SOCKET listenSock = reinterpret_cast<SOCKET>(mSocketPtr);
        closesocket(listenSock);
        mSocketPtr = nullptr;

        std::lock_guard<std::mutex> lockGuard(mClientsMutex);
        for (void* c : mClients) {
            closesocket(reinterpret_cast<SOCKET>(c));
        }
        mClients.clear();
        WSACleanup();
    }

    bool DebugIPCServer::sendMessage(uint16_t messageType, const std::vector<uint8_t>& data) {
        return sendMessage(messageType, data.data(), data.size());
    }

    bool DebugIPCServer::sendMessage(uint16_t messageType) {
        return sendMessage(messageType, nullptr, 0);
    }

    bool DebugIPCServer::sendMessage(uint16_t messageType, std::string_view data) {
        return sendMessage(messageType, reinterpret_cast<const uint8_t*>(data.data()), data.size());
    }

    bool DebugIPCServer::sendMessage(uint16_t messageType, const uint8_t* data, size_t length) {
        if (!mSocketPtr) {
            return false;
        }

        // 构造消息：[type(2 bytes)大端 | length(4 bytes)大端 | bytes...]
        std::vector<uint8_t> buffer(6 + length);
        buffer[0] = static_cast<uint8_t>(messageType >> 8);
        buffer[1] = static_cast<uint8_t>(messageType & 0xFF);
        uint32_t len = static_cast<uint32_t>(length);
        buffer[2] = (len >> 24) & 0xFF;
        buffer[3] = (len >> 16) & 0xFF;
        buffer[4] = (len >> 8) & 0xFF;
        buffer[5] = len & 0xFF;
        if (length > 0) {
            memcpy(buffer.data() + 6, data, length);
        }

        std::lock_guard<std::mutex> lockGuard(mClientsMutex);

        for (auto it = mClients.begin(); it != mClients.end(); ) {
            SOCKET clientSock = reinterpret_cast<SOCKET>(*it);
            int sent = send(clientSock, reinterpret_cast<const char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
            if (sent == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
                // 客户端断开或其他错误，移除客户端
                closesocket(clientSock);
                it = mClients.erase(it);
            } else {
                ++it;
            }
        }

        return true;
    }

    unsigned short DebugIPCServer::getPort() const {
        return mPort;
    }

    std::atomic<bool>* DebugIPCServer::getStopFlag() {
        return &mStopFlag;
    }

    void DebugIPCServer::join() {
        if(mThread.has_value() && mThread->joinable()) {
            mThread->join();
        }
    }

    void DebugIPCServer::safeExit() {
        stop();
        join();
    }

    void DebugIPCServer::detach() {
        if(mThread.has_value() && mThread->joinable()) {
            mThread->detach();
        }
    }

    std::thread* DebugIPCServer::getThread() {
        return mThread.has_value() ? &mThread.value() : nullptr;
    }
#endif

    std::shared_ptr<DebugIPCServer> createDebugServer() {
        return std::make_shared<DebugIPCServer>();
    }

    HotReloadWatcherTask::HotReloadWatcherTask(int processId, const std::vector<std::filesystem::path>& modDirs)
        : mProcessId(processId), mModDirs(modDirs) {
    }

    HotReloadWatcherTask::HotReloadWatcherTask(int processId, std::vector<std::filesystem::path>&& modDirs)
        : mProcessId(processId), mModDirs(std::move(modDirs)) {
    }

    void HotReloadWatcherTask::safeExit() {
        mStopFlag = true;
        join();
        fileWatcherThread.reset();
        processWatcherThread.reset();
    }
    
    void HotReloadWatcherTask::start() {
        // 实现启动逻辑
        if(fileWatcherThread.has_value() || processWatcherThread.has_value()) {
            throw std::runtime_error("Watcher threads already running");
        }
        mStopFlag = false;
        bool mNeedUpdate = false;
        bool mIsForeground = false;
        fileWatcherThread = MCDevTool::HotReload::watchAndReloadPyFiles(mModDirs, [this](const std::filesystem::path& path) {
            this->mNeedUpdate = true;
            this->onFileChanged(path);
            if(this->mIsForeground) {
                this->mNeedUpdate = false;
                this->onHotReloadTriggered();
            }
        }, &mStopFlag);
        if(!fileWatcherThread.has_value()) {
            fileWatcherThread.reset();
            throw std::runtime_error("Failed to start file watcher thread");
        }
        processWatcherThread = MCDevTool::HotReload::watchProcessForegroundWindow(mProcessId, [this](bool isForeground) {
            this->mIsForeground = isForeground;
            if(isForeground && this->mNeedUpdate) {
                this->mNeedUpdate = false;
                this->onHotReloadTriggered();
            }
        }, &mStopFlag);
        if(!processWatcherThread.has_value()) {
            processWatcherThread.reset();
            fileWatcherThread.reset();
            throw std::runtime_error("Failed to start process watcher thread");
        }
    }

    void HotReloadWatcherTask::stop() {
        // 实现停止逻辑
        if(fileWatcherThread.has_value()) {
            fileWatcherThread.reset();
        }
        if(processWatcherThread.has_value()) {
            processWatcherThread.reset();
        }
    }

    void HotReloadWatcherTask::join() {
        if(fileWatcherThread.has_value() && fileWatcherThread->joinable()) {
            fileWatcherThread->join();
        }
        if(processWatcherThread.has_value() && processWatcherThread->joinable()) {
            processWatcherThread->join();
        }
    }

    void HotReloadWatcherTask::setProcessId(int processId) {
        mProcessId = processId;
    }

    void HotReloadWatcherTask::setModDirs(const std::vector<std::filesystem::path>& modDirs) {
        mModDirs = modDirs;
    }

    void HotReloadWatcherTask::setModDirs(std::vector<std::filesystem::path>&& modDirs) {
        mModDirs = std::move(modDirs);
    }

    void HotReloadWatcherTask::onHotReloadTriggered() {}

    void HotReloadWatcherTask::onFileChanged(const std::filesystem::path& filePath) {}
} // namespace MCDevTool::Debug