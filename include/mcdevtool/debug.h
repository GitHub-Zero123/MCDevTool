#pragma once
#include <string_view>
#include <mutex>
#include <thread>
#include <optional>
#include <vector>
#include <memory>
#include <cstdint>
#include <atomic>
#include <filesystem>
#include <functional>

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
        void safeExit();
        void join();
        void detach();

        std::thread* getThread();

        bool sendMessage(uint16_t messageType, std::string_view data);
        bool sendMessage(uint16_t messageType, const std::vector<uint8_t>& data);
        bool sendMessage(uint16_t messageType, const uint8_t* data, size_t length);
        bool sendMessage(uint16_t messageType);

        std::atomic<bool>* getStopFlag();

        unsigned short getPort() const;
    private:
        unsigned short mPort = 0;
        void* mSocketPtr = nullptr;
        std::vector<void*> mClients;
        std::optional<std::thread> mThread;
        std::mutex mClientsMutex;
        std::atomic<bool> mStopFlag = false;
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
        bool mNeedUpdate = false;
        bool mIsForeground = false;

    private:
        int mProcessId = 0;
        std::optional<std::thread> processWatcherThread;
        std::optional<std::thread> fileWatcherThread;
        std::vector<std::filesystem::path> mModDirs;
        std::atomic<bool> mStopFlag = false;
    };
} // namespace MCDevTool::Debug