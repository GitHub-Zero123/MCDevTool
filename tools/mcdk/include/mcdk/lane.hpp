#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

// Lane：给每个游戏实例一个自己的数据根目录，按级别决定哪些数据独享、哪些与真实目录共享。
// 不是沙盒，没有安全边界，只改变游戏取到的 AppData。机制与取舍见 docs/lane.md。
namespace mcdk::lane {

    enum class Level : std::uint8_t {
        Addons, // 只独享包目录
        Worlds, // 独享包目录和存档
        Full,   // 整个数据目录都独享
    };

    [[nodiscard]] std::optional<Level> parseLevel(std::string_view name) noexcept;
    [[nodiscard]] std::string_view     levelName(Level level) noexcept;

    // 默认放在工程里，跟着工程或 worktree 走。
    [[nodiscard]] std::filesystem::path defaultRoot(const std::filesystem::path& project, std::string_view session);

    // 按级别建好目录布局，root 随后作为游戏的 Roaming AppData。
    // 共享部分用目录联接指回 realAppData；已存在的真实目录绝不删除。级别与上次不同时抛异常。
    void prepare(const std::filesystem::path& realAppData, const std::filesystem::path& root, Level level);

    // 删除 Lane。遇到联接只摘链接，不进入，联接指向的真实数据不受影响。
    void remove(const std::filesystem::path& root);

    // lane.json 里记下的级别；目录不是 Lane 时为空。
    [[nodiscard]] std::optional<Level> readLevel(const std::filesystem::path& root);

    // 游戏的 Python 加载器把模组脚本路径按 '/' 切开后直接丢掉前 10 段，剩下的拼成模块名
    // （ModSDK mod/common/minecraftMod.py 中 LoadWindowsAddonPy 等处的 split('/')[10:]）。
    // 所以游戏看到的 AppData 必须正好 5 段。工程里的 Lane 目录做不到，游戏经由一个 5 段的入口访问它。
    inline constexpr std::size_t GameAppDataSegments = 5;

    // 路径段数，盘符算一段。
    [[nodiscard]] std::size_t pathSegments(const std::filesystem::path& path);

    // %USERPROFILE%\.mcdk-lanes，槽位是它的子目录，正好 5 段。用户目录段数不符时抛异常。
    [[nodiscard]] std::filesystem::path defaultEntryDirectory();

    // 入口槽位：directory\<编号> 是指向 Lane 目录的联接。
    // 占用期间持有以槽位命名的系统互斥量，进程退出（含被强杀）时由内核释放，空闲与否永远准确。
    // 入口数量不会超过同时运行的实例数，空闲槽位的联接在下次占用或 sweepEntries 时摘掉。
    class EntrySlot {
    public:
        EntrySlot(const std::filesystem::path& directory, const std::filesystem::path& root);
        ~EntrySlot();
        EntrySlot(const EntrySlot&)            = delete;
        EntrySlot& operator=(const EntrySlot&) = delete;

        [[nodiscard]] const std::filesystem::path& path() const noexcept { return mPath; }

    private:
        void*                 mMutex = nullptr;
        std::filesystem::path mPath;
    };

    // 摘掉空闲槽位留下的联接；正在使用的槽位和不是联接的条目都不动。
    void sweepEntries(const std::filesystem::path& directory);

    // 在挂起的游戏进程里排队加载 shim。恢复主线程后调用 waitArmed 确认它已经布防。
    class ShimInjection {
    public:
        ShimInjection(void* process, void* mainThread, std::uint32_t processId, const std::filesystem::path& shim);
        ~ShimInjection();
        ShimInjection(const ShimInjection&)            = delete;
        ShimInjection& operator=(const ShimInjection&) = delete;

        // 超时或游戏先退出都抛异常，调用方负责结束游戏。
        void waitArmed(std::chrono::milliseconds timeout) const;

    private:
        void* mProcess = nullptr;
        void* mArmed   = nullptr;
    };

    // mcdk 旁边的 mcdev-lane-shim.dll。
    [[nodiscard]] std::filesystem::path shimPath(const std::filesystem::path& executableDirectory);

} // namespace mcdk::lane
