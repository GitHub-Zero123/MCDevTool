#include <mcdk/lane.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <fstream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>

#include <mcdevtool/env.h>
#include <mcdevtool/utils.h>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <mcdev_lane_shim.h>
#endif

namespace mcdk::lane {
    namespace fs = std::filesystem;

    namespace {
        constexpr std::array LevelNames{std::string_view{"addons"}, std::string_view{"worlds"}, std::string_view{"full"}};
        constexpr auto       DataDirectory = "MinecraftPE_Netease";
        constexpr auto       MetadataFile  = "lane.json";

        std::string utf8(const fs::path& path) { return MCDevTool::Utils::pathToGenericUtf8(path); }

        using MCDevTool::isLink;

        bool pathExists(const fs::path& path) {
            std::error_code error;
            return fs::symlink_status(path, error).type() != fs::file_type::not_found && !error;
        }

        // 只在目标不存在或本来就是联接时才建联接；真实目录说明这份数据已经是 Lane 私有的，保留。
        void shareDirectory(const fs::path& real, const fs::path& link) {
            if (pathExists(link) && !isLink(link)) {
                return;
            }
            if (!MCDevTool::createDirectoryJunction(real, link)) {
                throw std::runtime_error("无法创建 Lane 联接：" + utf8(link));
            }
        }

        // 文件没法跨盘共享，初次复制一份，之后归 Lane 所有。
        void seedFile(const fs::path& real, const fs::path& target) {
            if (!pathExists(target)) {
                fs::copy_file(real, target);
            }
        }

        // 把 real 下的条目映射进 lane；private 里的名字留给 Lane 自己。
        void mirror(const fs::path& real, const fs::path& lane, std::initializer_list<std::string_view> privateNames) {
            fs::create_directories(lane);
            std::error_code error;
            for (const auto& entry : fs::directory_iterator(real, error)) {
                const auto name = utf8(entry.path().filename());
                if (std::ranges::find(privateNames, name) != privateNames.end()) {
                    continue;
                }
                if (entry.is_directory()) {
                    shareDirectory(entry.path(), lane / entry.path().filename());
                } else if (entry.is_regular_file()) {
                    seedFile(entry.path(), lane / entry.path().filename());
                }
            }
        }

        void checkLevel(const fs::path& root, Level level) {
            const auto metadata = root / MetadataFile;
            if (fs::is_regular_file(metadata)) {
                std::ifstream input(metadata, std::ios::binary);
                const auto    json     = nlohmann::json::parse(input, nullptr, false);
                const auto    previous = json.is_object() ? json.value("level", "") : std::string{};
                if (previous != levelName(level)) {
                    throw std::runtime_error(
                        "Lane " + utf8(root) + " 之前的级别是 " + previous + "，不能改成 " + std::string(levelName(level))
                        + "。先用 mcdk lane remove 删除这个 Lane。"
                    );
                }
                return;
            }
            fs::create_directories(root);
            std::ofstream(metadata, std::ios::binary) << nlohmann::json{{"level", levelName(level)}}.dump(2);
        }
    } // namespace

    std::optional<Level> parseLevel(std::string_view name) noexcept {
        const auto found = std::ranges::find(LevelNames, name);
        return found == LevelNames.end() ? std::nullopt
                                         : std::optional<Level>{static_cast<Level>(found - LevelNames.begin())};
    }

    std::string_view levelName(Level level) noexcept { return LevelNames[static_cast<std::size_t>(level)]; }

    fs::path defaultRoot(const fs::path& project, std::string_view session) {
        return project / ".mcdev" / "lanes" / fs::u8path(std::string(session));
    }

    void prepare(const fs::path& realAppData, const fs::path& root, Level level) {
        checkLevel(root, level);
        const auto real = realAppData / DataDirectory;
        const auto lane = root / DataDirectory;
        fs::create_directories(lane);
        if (level == Level::Full) {
            return;
        }

        // 包目录始终独享：mcdk 每次启动都会清空并重建它们。
        const auto packsReal = real / "games" / "com.netease";
        const auto packsLane = lane / "games" / "com.netease";
        if (level == Level::Worlds) {
            mirror(real, lane, {"games", "minecraftWorlds"});
            fs::create_directories(lane / "minecraftWorlds");
        } else {
            mirror(real, lane, {"games"});
        }
        mirror(real / "games", lane / "games", {"com.netease"});
        mirror(packsReal, packsLane, {"behavior_packs", "resource_packs", "_dependencies_packs"});
        fs::create_directories(packsLane / "behavior_packs");
        fs::create_directories(packsLane / "resource_packs");
    }

    void remove(const fs::path& root) { MCDevTool::removeAllNoFollow(root); }

    std::size_t pathSegments(const fs::path& path) {
        std::size_t count = 0;
        for (const auto& part : fs::absolute(path).lexically_normal()) {
            if (!part.empty() && part != part.root_directory() && part.native() != L"\\" && part.native() != L"/") {
                ++count;
            }
        }
        return count;
    }

#ifdef _WIN32
    namespace {
        constexpr int EntrySlotCount = 64;

        std::mutex& ownedLock() {
            static std::mutex lock;
            return lock;
        }

        // 本进程占着的槽位。命名互斥量对同一线程可重入，不能只靠它判断。
        std::set<fs::path>& ownedSlots() {
            static std::set<fs::path> slots;
            return slots;
        }

        bool ownedHere(const fs::path& slot) {
            std::lock_guard guard(ownedLock());
            return ownedSlots().contains(slot);
        }

        std::wstring slotMutexName(const fs::path& directory, int slot) {
            std::uint64_t hash = 14695981039346656037ull;
            for (const unsigned char byte : utf8(fs::absolute(directory).lexically_normal())) {
                hash = (hash ^ static_cast<unsigned char>(std::tolower(byte))) * 1099511628211ull;
            }
            return std::format(L"Local\\mcdk-lane-entry-{:016x}-{}", hash, slot);
        }

        // 拿到就返回句柄，被别的进程占着返回 nullptr。被弃置的互斥量说明上一个占用者已经退出，可以接手。
        HANDLE tryLock(const fs::path& directory, int slot) {
            HANDLE mutex = CreateMutexW(nullptr, FALSE, slotMutexName(directory, slot).c_str());
            if (mutex == nullptr) return nullptr;
            const DWORD result = WaitForSingleObject(mutex, 0);
            if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) return mutex;
            CloseHandle(mutex);
            return nullptr;
        }

        void unlock(HANDLE mutex) {
            ReleaseMutex(mutex);
            CloseHandle(mutex);
        }
    } // namespace

    fs::path defaultEntryDirectory() {
        const DWORD length = GetEnvironmentVariableW(L"USERPROFILE", nullptr, 0);
        std::wstring profile(length, L'\0');
        profile.resize(GetEnvironmentVariableW(L"USERPROFILE", profile.data(), length));
        const auto directory = fs::path(profile) / ".mcdk-lanes";
        if (profile.empty() || pathSegments(directory) != GameAppDataSegments - 1) {
            throw std::runtime_error(std::format(
                "Lane 入口需要正好 {} 段的路径，{} 下的槽位不满足：游戏的模组加载器写死了路径层数",
                GameAppDataSegments,
                utf8(directory)
            ));
        }
        return directory;
    }

    EntrySlot::EntrySlot(const fs::path& directory, const fs::path& root) {
        fs::create_directories(directory);
        for (int slot = 0; slot < EntrySlotCount && mMutex == nullptr; ++slot) {
            const auto path = directory / std::to_string(slot);
            if (ownedHere(path)) continue;
            HANDLE mutex = tryLock(directory, slot);
            if (mutex == nullptr) continue;
            // 槽位上是真实目录说明不是 mcdk 建的，绝不删除。
            if (pathExists(path) && !isLink(path)) {
                unlock(mutex);
                continue;
            }
            if (!MCDevTool::createDirectoryJunction(root, path)) {
                unlock(mutex);
                throw std::runtime_error("无法创建 Lane 入口：" + utf8(path));
            }
            mMutex = mutex;
            mPath  = path;
            std::lock_guard guard(ownedLock());
            ownedSlots().insert(path);
        }
        if (mMutex == nullptr) {
            throw std::runtime_error("没有空闲的 Lane 入口槽位：" + utf8(directory));
        }
        sweepEntries(directory);
    }

    EntrySlot::~EntrySlot() {
        if (mMutex == nullptr) return;
        {
            std::lock_guard guard(ownedLock());
            ownedSlots().erase(mPath);
        }
        unlock(mMutex);
    }

    void sweepEntries(const fs::path& directory) {
        for (int slot = 0; slot < EntrySlotCount; ++slot) {
            const auto path = directory / std::to_string(slot);
            if (!pathExists(path) || !isLink(path) || ownedHere(path)) continue;
            if (HANDLE mutex = tryLock(directory, slot)) {
                MCDevTool::removeAllNoFollow(path);
                unlock(mutex);
            }
        }
    }
#else
    fs::path defaultEntryDirectory() { throw std::runtime_error("Lane 仅支持 Windows"); }
    EntrySlot::EntrySlot(const fs::path&, const fs::path&) { throw std::runtime_error("Lane 仅支持 Windows"); }
    EntrySlot::~EntrySlot() = default;
    void sweepEntries(const fs::path&) {}
#endif

    fs::path shimPath(const fs::path& executableDirectory) { return executableDirectory / "mcdev-lane-shim.dll"; }

#ifdef _WIN32
    ShimInjection::ShimInjection(void* process, void* mainThread, std::uint32_t processId, const fs::path& shim)
    : mProcess(process) {
        if (!fs::is_regular_file(shim)) {
            throw std::runtime_error("缺少 Lane 组件 " + utf8(shim) + "，无法隔离数据目录，启动中止");
        }
        wchar_t name[64];
        swprintf_s(name, MCDEV_LANE_ARMED_EVENT_FORMAT, static_cast<unsigned long>(processId));
        mArmed = CreateEventW(nullptr, TRUE, FALSE, name);
        if (mArmed == nullptr) {
            throw std::runtime_error("无法创建 Lane 就绪事件");
        }

        const std::wstring path  = shim.wstring();
        const SIZE_T       bytes = (path.size() + 1) * sizeof(wchar_t);
        void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        const auto loadLibrary =
            reinterpret_cast<PAPCFUNC>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        // 主线程恢复后、走到入口点之前执行这个 APC，加壳代码那时还没运行。
        if (remote == nullptr || !WriteProcessMemory(process, remote, path.c_str(), bytes, nullptr)
            || loadLibrary == nullptr || !QueueUserAPC(loadLibrary, mainThread, reinterpret_cast<ULONG_PTR>(remote))) {
            throw std::runtime_error("无法向游戏注入 Lane 组件（错误 " + std::to_string(GetLastError()) + "）");
        }
    }

    ShimInjection::~ShimInjection() {
        if (mArmed != nullptr) {
            CloseHandle(mArmed);
        }
    }

    void ShimInjection::waitArmed(std::chrono::milliseconds timeout) const {
        const HANDLE handles[]{mArmed, mProcess};
        const DWORD  result = WaitForMultipleObjects(2, handles, FALSE, static_cast<DWORD>(timeout.count()));
        if (result == WAIT_OBJECT_0) {
            return;
        }
        if (result == WAIT_OBJECT_0 + 1) {
            DWORD exitCode = 0;
            GetExitCodeProcess(mProcess, &exitCode);
            const char* reason = exitCode == MCDEV_LANE_EXIT_SETUP_FAILED ? "shim 初始化失败"
                               : exitCode == MCDEV_LANE_EXIT_HOOK_FAILED  ? "shim 无法安装重定向"
                                                                          : "游戏在 Lane 布防前退出";
            throw std::runtime_error(std::format("Lane 隔离未生效：{}（退出码 {:#010x}）", reason, exitCode));
        }
        throw std::runtime_error("Lane 组件没有在限定时间内就绪，启动中止");
    }
#else
    ShimInjection::ShimInjection(void*, void*, std::uint32_t, const fs::path&) {
        throw std::runtime_error("Lane 仅支持 Windows");
    }
    ShimInjection::~ShimInjection() = default;
    void ShimInjection::waitArmed(std::chrono::milliseconds) const {}
#endif

} // namespace mcdk::lane
