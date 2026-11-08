#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

// mcdk run 的会话记录。由运行中的 mcdk 写入，其他 mcdk 命令读取，格式见 docs/cli.md。
namespace mcdk {

    // mcdk stop 用这个退出码结束游戏，以区别于崩溃。
    inline constexpr std::uint32_t SessionStopExitCode = 0x4D43444B;

    inline constexpr std::string_view SessionStateReady  = "game_ready";
    inline constexpr std::string_view SessionStateExited = "exited";
    // 记录未写完 exited，mcdk 进程却已不在。
    inline constexpr std::string_view SessionStateLost = "lost";

    struct SessionRecord {
        std::string                  name;
        std::string                  state;
        std::uint32_t                mcdkPid       = 0;
        std::uint64_t                mcdkStartTime = 0;
        std::uint32_t                gamePid       = 0;
        std::string                  worldFolderName;
        std::string                  mcpHost;
        int                          mcpPort = 0;
        std::string                  lane; // Lane 级别，空表示没有隔离
        std::string                  laneRoot;
        std::string                  configFingerprint; // run --reuse 据此判断能否复用
        std::uint32_t                readyCount = 0;    // 进入存档的次数，重载存档后加一
        std::optional<std::uint32_t> exitCode;
    };

    [[nodiscard]] bool isValidSessionName(std::string_view name) noexcept;

    [[nodiscard]] std::filesystem::path sessionsDirectory(const std::filesystem::path& projectRoot);
    [[nodiscard]] std::filesystem::path
    sessionRecordPath(const std::filesystem::path& projectRoot, std::string_view name);

    // 原子替换，读取方不会看到写了一半的文件。
    void writeSessionRecord(const std::filesystem::path& path, const SessionRecord& record);
    [[nodiscard]] std::optional<SessionRecord> readSessionRecord(const std::filesystem::path& path);
    [[nodiscard]] std::vector<SessionRecord>   listSessionRecords(const std::filesystem::path& projectRoot);

    [[nodiscard]] nlohmann::ordered_json sessionRecordToJson(const SessionRecord& record);

    // 按 pid 和启动时间判断，pid 被复用时不会误判。
    [[nodiscard]] bool          isSessionAlive(const SessionRecord& record);
    // 进程已不在时返回 SessionStateLost，其余返回记录里的状态。
    [[nodiscard]] std::string   effectiveSessionState(const SessionRecord& record);
    [[nodiscard]] std::uint64_t currentProcessStartTime();

    // 向系统要一个当前空闲的本地端口。
    [[nodiscard]] int allocateLocalPort(const std::string& host);

    // 运行中的 mcdk 用它维护自己的记录，可跨线程调用。未 start 之前的更新不落盘。
    class SessionRecordWriter {
    public:
        SessionRecordWriter(std::filesystem::path path, SessionRecord record);

        void start(std::uint32_t gamePid, std::string_view state);
        void setState(std::string_view state);
        void finish(std::uint32_t exitCode);

    private:
        void flush();

        std::mutex            mMutex;
        std::filesystem::path mPath;
        SessionRecord         mRecord;
        bool                  mStarted = false;
    };

} // namespace mcdk
