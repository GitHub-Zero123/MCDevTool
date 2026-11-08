#include <mcdk/lane.hpp>

#include <mcdevtool/env.h>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {
    namespace fs   = std::filesystem;
    namespace lane = mcdk::lane;

    void require(bool condition, const std::string& message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    // 用 MCDevTool::isLink：file_type::junction 只有 MSVC STL 才会报告。
    bool isJunction(const fs::path& path) { return MCDevTool::isLink(path); }

    bool isPrivateDirectory(const fs::path& path) { return !MCDevTool::isLink(path) && fs::is_directory(path); }

    bool throws(const std::function<void()>& action) {
        try {
            action();
        } catch (const std::exception&) {
            return true;
        }
        return false;
    }

    void write(const fs::path& path, const std::string& text) {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << text;
    }

    // 模拟真实的 AppData：设置、日志、存档、全局包目录和散落的文件。
    fs::path makeRealAppData(const fs::path& base) {
        const auto data = base / "MinecraftPE_Netease";
        write(data / "minecraftpe" / "options.txt", "real options");
        write(data / "logs" / "a.txt", "log");
        write(data / "minecraftWorlds" / "w1" / "level.dat", "real world");
        write(data / "games" / "com.netease" / "behavior_packs" / "p" / "manifest.json", "{}");
        write(data / "games" / "com.netease" / "resource_packs" / "r" / "manifest.json", "{}");
        write(data / "games" / "com.netease" / "minecraftpe" / "x.txt", "x");
        write(data / "deviceid.txt", "device");
        return base;
    }

    void testFull(const fs::path& real, const fs::path& root) {
        lane::prepare(real, root / "full", lane::Level::Full);
        const auto data = root / "full" / "MinecraftPE_Netease";
        require(isPrivateDirectory(data), "full creates the data directory");
        require(fs::is_empty(data), "full shares nothing");
    }

    void testWorlds(const fs::path& real, const fs::path& root) {
        const auto laneRoot = root / "worlds";
        lane::prepare(real, laneRoot, lane::Level::Worlds);
        const auto data = laneRoot / "MinecraftPE_Netease";
        require(isJunction(data / "minecraftpe"), "settings are shared");
        require(isPrivateDirectory(data / "logs") && fs::is_empty(data / "logs"), "logs are private");
        require(isPrivateDirectory(data / "minecraftWorlds"), "worlds are private");
        require(fs::is_empty(data / "minecraftWorlds"), "private worlds start empty");
        require(fs::is_regular_file(data / "deviceid.txt"), "loose files are copied");
        const auto packs = data / "games" / "com.netease";
        require(isPrivateDirectory(packs / "behavior_packs") && fs::is_empty(packs / "behavior_packs"), "packs are private");
        require(isPrivateDirectory(packs / "resource_packs"), "resource packs are private");
        require(isJunction(packs / "minecraftpe"), "other netease data is shared");

        // 再次准备不应改变布局，Lane 里写过的私有数据也要保留。
        write(data / "minecraftWorlds" / "mine" / "level.dat", "lane world");
        lane::prepare(real, laneRoot, lane::Level::Worlds);
        require(fs::is_regular_file(data / "minecraftWorlds" / "mine" / "level.dat"), "private data survives");

        require(throws([&] { lane::prepare(real, laneRoot, lane::Level::Full); }), "level cannot change silently");
        require(lane::readLevel(laneRoot) == lane::Level::Worlds, "the level is recorded");
        require(!lane::readLevel(root / "missing").has_value(), "a plain directory has no level");
    }

    void testAddons(const fs::path& real, const fs::path& root) {
        const auto laneRoot = root / "addons";
        lane::prepare(real, laneRoot, lane::Level::Addons);
        const auto data = laneRoot / "MinecraftPE_Netease";
        require(isJunction(data / "minecraftWorlds"), "worlds are shared");
        require(fs::is_regular_file(data / "minecraftWorlds" / "w1" / "level.dat"), "shared worlds are visible");
        require(isPrivateDirectory(data / "games" / "com.netease" / "behavior_packs"), "packs stay private");
    }

    void testOldSharedLogsBecomePrivate(const fs::path& real, const fs::path& root) {
        // 旧版本的 worlds Lane 里 logs 是联接；再次准备时只摘联接，真实日志不能被删。
        const auto laneRoot = root / "oldlogs";
        lane::prepare(real, laneRoot, lane::Level::Worlds);
        const auto logs = laneRoot / "MinecraftPE_Netease" / "logs";
        fs::remove(logs);
        require(MCDevTool::createDirectoryJunction(real / "MinecraftPE_Netease" / "logs", logs), "old junction");
        lane::prepare(real, laneRoot, lane::Level::Worlds);
        require(isPrivateDirectory(logs) && fs::is_empty(logs), "the junction is replaced by a private directory");
        require(fs::is_regular_file(real / "MinecraftPE_Netease" / "logs" / "a.txt"), "real logs survive");
        lane::remove(laneRoot);
    }

    void testExistingPrivateDirectoryIsKept(const fs::path& real, const fs::path& root) {
        // 游戏在上次运行里自己建了 logs 时，那份数据属于 Lane，不能被换成联接。
        const auto laneRoot = root / "kept";
        write(laneRoot / "MinecraftPE_Netease" / "logs" / "lane.txt", "lane log");
        lane::prepare(real, laneRoot, lane::Level::Worlds);
        require(isPrivateDirectory(laneRoot / "MinecraftPE_Netease" / "logs"), "private directory is not replaced");
        require(fs::is_regular_file(laneRoot / "MinecraftPE_Netease" / "logs" / "lane.txt"), "private file survives");
    }

    void testRemoveNeverFollowsLinks(const fs::path& real, const fs::path& root) {
        for (const auto* name : {"worlds", "addons", "full", "kept"}) {
            lane::remove(root / name);
            require(!fs::exists(root / name), std::string("lane is removed: ") + name);
        }
        const auto data = real / "MinecraftPE_Netease";
        require(fs::is_regular_file(data / "minecraftpe" / "options.txt"), "shared settings survive removal");
        require(fs::is_regular_file(data / "minecraftWorlds" / "w1" / "level.dat"), "shared worlds survive removal");
        require(fs::is_regular_file(data / "games" / "com.netease" / "minecraftpe" / "x.txt"), "shared data survives");
        lane::remove(root / "missing");
    }

    bool pointsTo(const fs::path& link, const fs::path& target) {
        return MCDevTool::isLink(link) && fs::equivalent(link, target);
    }

    void testEntrySlots(const fs::path& base) {
        const auto directory = base / "entries";
        const auto laneA     = base / "laneA";
        const auto laneB     = base / "laneB";
        fs::create_directories(laneA);
        fs::create_directories(laneB);

        // 槽位上已有的真实目录不是 mcdk 建的，要跳过并保留。
        write(directory / "0" / "user.txt", "keep");

        std::optional<lane::EntrySlot> first;
        first.emplace(directory, laneA);
        require(first->path() == directory / "1", "a real directory is skipped");
        require(pointsTo(first->path(), laneA), "the slot links to the lane");

        {
            lane::EntrySlot second(directory, laneB);
            require(second.path() == directory / "2", "a held slot is not reused");
            require(pointsTo(first->path(), laneA), "the other slot is untouched");
        }
        require(MCDevTool::isLink(directory / "2"), "a released slot keeps its link until swept");

        {
            lane::EntrySlot third(directory, laneB);
            require(third.path() == directory / "2", "a free slot is reused instead of growing");
        }

        first.reset();
        lane::sweepEntries(directory);
        require(!fs::exists(directory / "1") && !fs::exists(directory / "2"), "free slots are swept");
        require(fs::is_regular_file(directory / "0" / "user.txt"), "real directories survive the sweep");
        require(fs::is_directory(laneA) && fs::is_directory(laneB), "sweeping keeps the lanes");
    }

    void testSegments() {
        require(lane::pathSegments(fs::u8path("C:/Users/用户/AppData/Roaming")) == lane::GameAppDataSegments, "Roaming");
        require(lane::pathSegments(fs::u8path("C:\\Users\\用户\\.mcdk-lanes\\0")) == lane::GameAppDataSegments, "slot");
        require(lane::pathSegments(fs::u8path("C:/Users/用户/.mcdk-lanes")) == 4, "entry directory");
    }

    void testLevels() {
        require(lane::parseLevel("worlds") == lane::Level::Worlds, "parse worlds");
        require(!lane::parseLevel("World").has_value(), "names are exact");
        require(lane::levelName(lane::Level::Addons) == "addons", "level name");
        require(
            lane::defaultRoot(fs::u8path("D:/工程"), "a") == fs::u8path("D:/工程") / ".mcdev" / "lanes" / "a",
            "default root lives in the project"
        );
    }
} // namespace

int main() {
    const auto base = fs::temp_directory_path() / fs::u8path("mcdk_Lane 布局_测试");
    try {
        fs::remove_all(base);
        const auto real  = makeRealAppData(base / "real");
        const auto lanes = base / "lanes";
        testLevels();
        testFull(real, lanes);
        testWorlds(real, lanes);
        testAddons(real, lanes);
        testExistingPrivateDirectoryIsKept(real, lanes);
        testOldSharedLogsBecomePrivate(real, lanes);
        testEntrySlots(lanes / "slots");
        testSegments();
        testRemoveNeverFollowsLinks(real, lanes);
        fs::remove_all(base);
    } catch (const std::exception& exception) {
        std::cerr << "FAILED: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "lane layout tests passed\n";
    return EXIT_SUCCESS;
}
