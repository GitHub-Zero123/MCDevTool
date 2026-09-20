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

#include <nlohmann/json_fwd.hpp>

namespace MCDevTool::Debug {
    inline constexpr uint16_t IPC_JSON_REQUEST_TYPE  = 100;
    inline constexpr uint16_t IPC_JSON_RESPONSE_TYPE = 101;

    struct IPCJsonResult {
        bool        success   = false; // 仅表示 IPC request/response 是否成功完成，不代表业务 ok 字段
        bool        timeout   = false;
        uint64_t    requestId = 0;
        std::string responseJson;
        std::string errorMessage;
        // Reuse the DOM parsed for response routing so callers do not parse a large response a second time.
        std::shared_ptr<nlohmann::json> responseValue;
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
        IPCJsonResult requestJsonValue(std::string_view method, nlohmann::json params, uint32_t timeoutMs = 10000);
        IPCJsonResult requestJsonRaw(std::string_view requestJson, uint32_t timeoutMs = 10000);

        // 获取链接的客户端数量
        size_t getClientCount() const;

        // 客户端连上 / 断开时的回调，参数是变化后的客户端数与本次变化的方向。
        //
        // 本层不认识插件系统（mcdevtool 是 mcdk_runtime 的上游），所以只提供钩子，
        // 由 mcdk::runtime 那一层去发 mcdk.ipc.client.* 事件。
        //
        // **必须在 start() 之前调用。** 该字段没有锁保护，靠的是「设置发生在
        // accept 线程创建之前」这一 happens-before；start() 之后再设就是对
        // std::function 的数据竞争。
        //
        // 回调跑在 accept 线程或客户端读线程上，实现必须线程安全且快——它阻塞的是
        // 接受新连接或读取现有连接。
        //
        // stop() 不会为被它清空的客户端触发断开回调：那时插件多半已经终结，
        // 通知也没有去处。插件应把 mcdk.game.exit 当作终止信号，而不是指望
        // 收齐每一次 disconnected。
        void setClientCountChangedCallback(std::function<void(std::size_t, bool)> callback);

        std::atomic<bool>* getStopFlag();

        unsigned short getPort() const;

    private:
        struct PendingJsonRequest {
            std::mutex                     mutex;
            std::condition_variable        cv;
            bool                           completed = false;
            bool                           retainResponseValue = false;
            std::string                    responseJson;
            std::shared_ptr<nlohmann::json> responseValue;
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
        std::function<void(std::size_t, bool)>              mClientCountChanged;
        void notifyClientCountChanged(std::size_t count, bool connected) const;
        bool sendMessageToOneClient(uint16_t messageType, const uint8_t* data, size_t length);
        bool sendBufferToSocket(void* socketPtr, const uint8_t* data, size_t length);
        IPCJsonResult requestJsonRawWithId(
            std::string_view requestJson,
            uint64_t         requestId,
            uint32_t         timeoutMs,
            bool             retainResponseValue
        );
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

        virtual ~HotReloadWatcherTask();

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

        std::mutex mStateMutex;
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
