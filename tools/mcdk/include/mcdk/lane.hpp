#pragma once

#include <chrono>
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

    // 游戏按真实 Roaming 的路径段数推算 Python 模组的模块名，Lane 路径段数不同就会加载失败。
    // 所以游戏实际使用的是 Roaming 旁边的一个入口联接，它指向工程里的 Lane 目录。
    [[nodiscard]] std::filesystem::path
    gameEntry(const std::filesystem::path& realAppData, const std::filesystem::path& root, std::string_view session);

    // 建好或更新入口联接。入口位置已有真实目录时抛异常，不会覆盖。
    void linkGameEntry(const std::filesystem::path& entry, const std::filesystem::path& root);

    // 只摘入口联接本身。
    void unlinkGameEntry(const std::filesystem::path& entry);

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
