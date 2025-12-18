#include "mcdevtool/env.h"
#include "mcdevtool/utils.h"
#include <cstdlib>
#include <utility>  // std::move
#include <iostream>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

// 获取所有逻辑驱动器
static std::vector<std::string> WIN32_GET_ALL_DRIVES() {
    char buffer[256];
    DWORD len = GetLogicalDriveStringsA(sizeof(buffer), buffer);

    std::vector<std::string> drives;
    char* p = buffer;
    while (*p) {
        drives.emplace_back(p);
        p += strlen(p) + 1;
    }
    return drives;
}

// 软链接目录
static bool CREATE_JUNCTION(const std::filesystem::path& target, const std::filesystem::path& link) {

    std::filesystem::create_directories(link.parent_path());
    if(std::filesystem::exists(link)) {
        std::filesystem::remove_all(link);
    }
    std::filesystem::create_directory(link);

    HANDLE h = CreateFileW(
        link.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);

    if(h == INVALID_HANDLE_VALUE) { return false; }

    std::wstring real = std::filesystem::absolute(target).wstring();
    std::wstring substitute = L"\\??\\" + real;

    struct REPARSE_DATA_BUFFER {
        DWORD ReparseTag;
        WORD  ReparseDataLength;
        WORD  Reserved;
        WORD  SubstituteNameOffset;
        WORD  SubstituteNameLength;
        WORD  PrintNameOffset;
        WORD  PrintNameLength;
        WCHAR PathBuffer[1];
    };

    const auto substLen = (substitute.size() * sizeof(WCHAR));
    const auto printLen = (real.size() * sizeof(WCHAR));

    const auto totalLen =
        FIELD_OFFSET(REPARSE_DATA_BUFFER, PathBuffer) +
        substLen + sizeof(WCHAR) +        // substitute + null
        printLen + sizeof(WCHAR);         // print + null

    std::vector<char> buffer(totalLen);
    auto* rp = reinterpret_cast<REPARSE_DATA_BUFFER*>(buffer.data());

    rp->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;
    rp->ReparseDataLength = WORD(totalLen - FIELD_OFFSET(REPARSE_DATA_BUFFER, SubstituteNameOffset));
    rp->Reserved = 0;
    rp->SubstituteNameOffset = 0;
    rp->SubstituteNameLength = WORD(substLen);
    rp->PrintNameOffset = WORD(substLen + sizeof(WCHAR));
    rp->PrintNameLength = WORD(printLen);

    memcpy(rp->PathBuffer, substitute.c_str(), substLen);
    rp->PathBuffer[substitute.size()] = L'\0';
    memcpy((PBYTE)rp->PathBuffer + substLen + sizeof(WCHAR), real.c_str(), printLen);
    rp->PathBuffer[substitute.size() + real.size() + 1] = L'\0';

    DWORD bytesReturned;
    BOOL ok = DeviceIoControl(h,
        FSCTL_SET_REPARSE_POINT,
        rp,
        totalLen,
        nullptr,
        0,
        &bytesReturned,
        nullptr);

    CloseHandle(h);
    return ok == TRUE;
}

#else
static bool CREATE_JUNCTION(const std::filesystem::path& target, const std::filesystem::path& link) = delete
#endif

static void normalizeUUIDString(std::string& uuidStr) {
    uuidStr.erase(std::remove(uuidStr.begin(), uuidStr.end(), '-'), uuidStr.end());
}

namespace MCDevTool {
    static std::filesystem::path appDataCachePath;

    // 获取应用数据目录路径
    std::filesystem::path getAppDataPath() {
        if(appDataCachePath.empty()) {
            const char* appData = std::getenv("APPDATA");
            if(appData) {
                appDataCachePath = std::filesystem::path(appData);
            }
        }
        return appDataCachePath;
    }

    // 获取MinecraftPE_Netease数据目录
    std::filesystem::path getMinecraftDataPath() {
        return getAppDataPath() / "MinecraftPE_Netease";
    }

    // 获取games/com.netease目录
    std::filesystem::path getGamesComNeteasePath() {
        return getMinecraftDataPath() / "games/com.netease";
    }

    // 获取minecraftWorlds目录
    std::filesystem::path getMinecraftWorldsPath() {
        return getMinecraftDataPath() / "minecraftWorlds";
    }

    // 获取行为包目录
    std::filesystem::path getBehaviorPacksPath() {
        return getGamesComNeteasePath() / "behavior_packs";
    }

    // 获取资源包目录
    std::filesystem::path getResourcePacksPath() {
        return getGamesComNeteasePath() / "resource_packs";
    }

    // 获取依赖包目录
    std::filesystem::path getDependenciesPacksPath() {
        // 依赖包并非官方目录 仅供开发工具使用
        return getGamesComNeteasePath() / "_dependencies_packs";
    }

#ifdef _WIN32
    static std::optional<std::filesystem::path> _gamePathCache;
    static bool _hasNoGamePath = false;

    // 自动搜索MCStudioDownload游戏路径（如果存在）
    std::optional<std::filesystem::path> autoSearchMCStudioDownloadGamePath() {
        if(_gamePathCache || _hasNoGamePath) {
            return _gamePathCache;
        }
        auto drives = WIN32_GET_ALL_DRIVES();
        for (const auto& drive : drives) {
            std::filesystem::path path = drive + "MCStudioDownload/game/MinecraftPE_Netease";
            if (std::filesystem::is_directory(path)) {
                // 遍历子文件夹 匹配存在Minecraft.Windows.exe
                for (const auto& entry : std::filesystem::directory_iterator(path)) {
                    if (entry.is_directory()) {
                        auto exePath = entry.path() / "Minecraft.Windows.exe";
                        if (std::filesystem::is_regular_file(exePath)) {
                            _gamePathCache = path;
                            return path;
                        }
                    }
                }
            }
        }
        _hasNoGamePath = true;
        return std::nullopt;
    }
#else
    // 自动搜索MCStudioDownload游戏路径（如果存在）
    std::optional<std::filesystem::path> autoSearchMCStudioDownloadGamePath() {
        return std::nullopt;
    }
#endif
    static std::optional<std::filesystem::path> _cacheLastestGamePath;

    // 自动匹配最新版本游戏可执行文件路径（如果存在）
    std::optional<std::filesystem::path> autoMatchLatestGameExePath() {
        if(_cacheLastestGamePath) {
            return _cacheLastestGamePath;
        }
        auto gamePath = autoSearchMCStudioDownloadGamePath();
        if(!gamePath) {
            return std::nullopt;
        }
        // 遍历子目录多个版本号文件夹
        std::optional<std::filesystem::path> latestExePath;
        MCDevTool::Utils::Version latestVersion;
        uint64_t latestVersionNum = 0;
        for (const auto& entry : std::filesystem::directory_iterator(gamePath.value())) {
            if (entry.is_directory()) {
                std::string folderName = entry.path().filename().string();
                // 文件夹版本号解析
                MCDevTool::Utils::Version ver {folderName};
                if(!ver) {
                    continue;
                }
                auto exePath = entry.path() / "Minecraft.Windows.exe";
                if (std::filesystem::is_regular_file(exePath)) {
                    // 比较版本号大小
                    if (latestVersionNum == 0 || latestVersion < ver) {
                        latestVersion = std::move(ver);
                        latestExePath = exePath;
                        latestVersionNum++;
                    }
                }
            }
        }
        if(latestExePath) {
            _cacheLastestGamePath = latestExePath;
        }
        return latestExePath;
    }

    // 清理运行时行为包目录
    void cleanRuntimeBehaviorPacks() {
        auto runtimeBPPath = getBehaviorPacksPath();
        if(std::filesystem::is_directory(runtimeBPPath)) {
            std::filesystem::remove_all(runtimeBPPath);
        }
    }

    // 清理运行时资源包目录
    void cleanRuntimeResourcePacks() {
        auto runtimeRPPath = getResourcePacksPath();
        if(std::filesystem::is_directory(runtimeRPPath)) {
            std::filesystem::remove_all(runtimeRPPath);
        }
    }

    // 同时清理双pack目录
    void cleanRuntimePacks() {
        cleanRuntimeBehaviorPacks();
        cleanRuntimeResourcePacks();
    }

    // 分析并link源代码addon目录到运行时行为/资源包目录
    Addon::PackInfo linkSourcePackToRuntimePack(const std::filesystem::path& sourceDir) {
        auto info = Addon::parsePackInfo(sourceDir);
        if(!info) { return info; }
        if(info.type == Addon::PackType::BEHAVIOR) {
            auto uuid = info.uuid;
            normalizeUUIDString(uuid);
            auto destPath = getBehaviorPacksPath() / uuid;
            if(!CREATE_JUNCTION(sourceDir, destPath)) {
                std::cerr << "行为包软链接创建失败: " << sourceDir.filename().string() << "\n";
            } else {
                info.path = destPath;
            }
        } else if(info.type == Addon::PackType::RESOURCE) {
            auto uuid = info.uuid;
            normalizeUUIDString(uuid);
            auto destPath = getResourcePacksPath() / uuid;
            if(!CREATE_JUNCTION(sourceDir, destPath)) {
                std::cerr << "资源包软链接创建失败: " << sourceDir.filename().string() << "\n";
            } else {
                info.path = destPath;
            }
        }
        return info;
    }

    // 分析并link源代码addon目录到运行时行为包目录 该版本支持资源+行为包组合
    std::vector<Addon::PackInfo> linkSourceAddonToRuntimePacks(const std::filesystem::path& sourceDir) {
        std::vector<Addon::PackInfo> packs;
        // 先尝试处理当前目录为单一pack
        auto oncePackInfo = linkSourcePackToRuntimePack(sourceDir);
        if(oncePackInfo) {
            packs.push_back(std::move(oncePackInfo));
            return packs;
        }
        // 遍历当前文件夹下的所有文件夹
        for(const auto& entry : std::filesystem::directory_iterator(sourceDir)) {
            if(entry.is_directory()) {
                auto packInfo = linkSourcePackToRuntimePack(entry.path());
                if(packInfo) {
                    packs.push_back(std::move(packInfo));
                }
            }
        }
        return packs;
    }
}