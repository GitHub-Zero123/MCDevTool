#include "mcdevtool/env.h"
#include <cstdlib>
#include <utility>  // std::move

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

// 无需管理员权限 软链接目录
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
            auto destPath = getBehaviorPacksPath() / sourceDir.filename();
            // std::filesystem::create_directories(destPath.parent_path());
            // if(!std::filesystem::is_symlink(destPath)) {
            //     std::filesystem::create_directory_symlink(sourceDir, destPath);
            // }
            CREATE_JUNCTION(sourceDir, destPath);
        } else if(info.type == Addon::PackType::RESOURCE) {
            auto destPath = getResourcePacksPath() / sourceDir.filename();
            // std::filesystem::create_directories(destPath.parent_path());
            // if(!std::filesystem::is_symlink(destPath)) {
            //     std::filesystem::create_directory_symlink(sourceDir, destPath);
            // }
            CREATE_JUNCTION(sourceDir, destPath);
        }
        return info;
    }

    // 分析并link源代码addon目录到运行时行为包目录 该版本支持资源+行为包组合
    std::vector<Addon::PackInfo> linkSourceAddonToRuntimePacks(const std::filesystem::path& sourceDir) {
        std::vector<Addon::PackInfo> packs;
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