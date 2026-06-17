#pragma once
#include <string_view>
#include <string>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <optional>
#include <vector>
#include <memory>
#include <cstdint>
#include <atomic>
#include <filesystem>
#include <functional>
#include <map>

namespace MCDevTool::Debug {
    inline constexpr uint16_t IPC_JSON_REQUEST_TYPE  = 100;
    inline constexpr uint16_t IPC_JSON_RESPONSE_TYPE = 101;

    struct IPCJsonResult {
        bool        success   = false; // 仅表示 IPC request/response 是否成功完成，不代表业务 ok 字段
        bool        timeout   = false;
        uint64_t    requestId = 0;
        std::string responseJson;
        std::string errorMessage;
    };

    class DebugIPCServer {
    public:
        DebugIPCServer() = default;
        ~DebugIPCServer();

        // 禁止复制和移动
        DebugIPCServer(const DebugIPCServer&)            = delete;
        DebugIPCServer& operator=(const DebugIPCServer&) = delete;
        DebugIPCServer(DebugIPCServer&&)                 = delete;
        DebugIPCServer& operator=(DebugIPCServer&&)      = delete;

        void start();
        void stop();
        void safeExit();
        void join();
        void detach();

        std::thread* getThread();

        // 发送消息到所有连接的客户端
        bool sendMessage(uint16_t messageType, std::string_view data);
        bool sendMessage(uint16_t messageType, const std::vector<uint8_t>& data);
        bool sendMessage(uint16_t messageType, const uint8_t* data, size_t length);
        bool sendMessage(uint16_t messageType);

        // 发送 JSON request 到一个已连接客户端并等待同 id 的 JSON response；默认 10 秒超时；API 内部吞掉异常并返回错误信息
        IPCJsonResult requestJson(std::string_view method, std::string_view paramsJson = "{}", uint32_t timeoutMs = 10000);
        IPCJsonResult requestJsonRaw(std::string_view requestJson, uint32_t timeoutMs = 10000);

        // 获取链接的客户端数量
        size_t getClientCount() const;

        std::atomic<bool>* getStopFlag();

        unsigned short getPort() const;

    private:
        struct PendingJsonRequest {
            std::mutex              mutex;
            std::condition_variable cv;
            bool                    completed = false;
            std::string             responseJson;
        };

        unsigned short                                      mPort      = 0;
        void*                                               mSocketPtr = nullptr;
        std::vector<void*>                                  mClients;
        std::optional<std::thread>                          mThread;
        std::vector<std::thread>                            mClientThreads;
        mutable std::mutex                                  mClientsMutex;
        std::mutex                                          mClientThreadsMutex;
        std::mutex                                          mSendMutex;
        std::mutex                                          mPendingJsonMutex;
        std::map<uint64_t, std::shared_ptr<PendingJsonRequest>> mPendingJsonRequests;
        std::atomic<uint64_t>                               mNextJsonRequestId = 1;
        std::atomic<bool>                                   mStopFlag = false;
        bool sendMessageToOneClient(uint16_t messageType, const uint8_t* data, size_t length);
        bool sendBufferToSocket(void* socketPtr, const uint8_t* data, size_t length);
        void clientReadLoop(void* socketPtr);
        void handleJsonResponsePacket(const uint8_t* data, size_t length);
        void eraseClient(void* socketPtr, bool closeSocket);
    };

    // 创建并返回一个DebugIPCServer的智能指针
    std::shared_ptr<DebugIPCServer> createDebugServer();

    class HotReloadWatcherTask {
    public:
        HotReloadWatcherTask() = default;
        HotReloadWatcherTask(int processId, const std::vector<std::filesystem::path>& modDirs);
        HotReloadWatcherTask(int processId, std::vector<std::filesystem::path>&& modDirs);

        virtual ~HotReloadWatcherTask() = default;

        void start();
        void stop();
        void safeExit();
        void join();
        void setProcessId(int processId);
        void setModDirs(const std::vector<std::filesystem::path>& modDirs);
        void setModDirs(std::vector<std::filesystem::path>&& modDirs);

        // 热更新触发（在文件修改后重新进入前台时调用）
        virtual void onHotReloadTriggered();

        // 文件更新触发（此时不一定在前台）
        virtual void onFileChanged(const std::filesystem::path& filePath);

    protected:
        virtual bool shouldWatchFile(const std::filesystem::path& filePath) const;

        bool mNeedUpdate   = false;
        bool mIsForeground = false;

    private:
        int                                mProcessId = 0;
        std::optional<std::thread>         processWatcherThread;
        std::optional<std::thread>         fileWatcherThread;
        std::vector<std::filesystem::path> mModDirs;
        std::atomic<bool>                  mStopFlag = false;
    };
} // namespace MCDevTool::Debug
