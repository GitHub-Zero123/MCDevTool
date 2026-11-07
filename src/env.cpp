#include "mcdevtool/env.h"
#include "mcdevtool/utils.h"
#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <utility> // std::move
#include <iostream>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

// 只有 MSVC STL 的 remove_all 保证不进入 junction，其他 STL（MinGW 的 libstdc++ 等）走自己的实现。
// 依据见 docs/junction-removal.md。
#ifdef _MSVC_STL_VERSION
bool MCDevTool::isLink(const std::filesystem::path& path) {
    std::error_code error;
    const auto      type = std::filesystem::symlink_status(path, error).type();
    return !error && (type == std::filesystem::file_type::junction || type == std::filesystem::file_type::symlink);
}

void MCDevTool::removeAllNoFollow(const std::filesystem::path& path, std::error_code& error) noexcept {
    std::filesystem::remove_all(path, error);
}
#else
namespace {
    // 不跟随链接读取条目本身的属性和 reparse tag；条目不存在时返回 false 且不置错误。
    bool queryEntry(const std::filesystem::path& path, FILE_ATTRIBUTE_TAG_INFO& info, std::error_code& error) {
        const HANDLE handle = CreateFileW(
            path.c_str(),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
            nullptr
        );
        if (handle == INVALID_HANDLE_VALUE) {
            const DWORD code = GetLastError();
            if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
                error.assign(static_cast<int>(code), std::system_category());
            }
            return false;
        }
        const BOOL ok = GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info));
        const DWORD code = GetLastError();
        CloseHandle(handle);
        if (!ok) {
            error.assign(static_cast<int>(code), std::system_category());
        }
        return ok == TRUE;
    }

    // 名称代理类 reparse point（junction、符号链接等）才算链接；OneDrive 占位目录等不算。
    bool isLinkInfo(const FILE_ATTRIBUTE_TAG_INFO& info) {
        return (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && IsReparseTagNameSurrogate(info.ReparseTag);
    }

    void setLastError(std::error_code& error) {
        error.assign(static_cast<int>(GetLastError()), std::system_category());
    }
} // namespace

bool MCDevTool::isLink(const std::filesystem::path& path) {
    FILE_ATTRIBUTE_TAG_INFO info{};
    std::error_code         error;
    return queryEntry(path, info, error) && isLinkInfo(info);
}

void MCDevTool::removeAllNoFollow(const std::filesystem::path& path, std::error_code& error) noexcept {
    error.clear();
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!queryEntry(path, info, error)) {
        return;
    }
    const bool directory = (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const bool link      = isLinkInfo(info);
    if (directory && !link) {
        WIN32_FIND_DATAW data;
        const HANDLE     find = FindFirstFileExW(
            (path / L"*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH
        );
        if (find == INVALID_HANDLE_VALUE) {
            setLastError(error);
            return;
        }
        do {
            const std::wstring_view name = data.cFileName;
            if (name == L"." || name == L"..") {
                continue;
            }
            removeAllNoFollow(path / data.cFileName, error);
        } while (!error && FindNextFileW(find, &data));
        if (!error && GetLastError() != ERROR_NO_MORE_FILES) {
            setLastError(error);
        }
        FindClose(find);
        if (error) {
            return;
        }
    }
    // 只读属性会挡住删除；链接上的属性改动会落到目标上，所以链接不碰。
    if (!link && (info.FileAttributes & FILE_ATTRIBUTE_READONLY)) {
        SetFileAttributesW(path.c_str(), info.FileAttributes & ~FILE_ATTRIBUTE_READONLY);
    }
    // 对 junction / 目录符号链接，RemoveDirectoryW 只删链接本身。
    if (!(directory ? RemoveDirectoryW(path.c_str()) : DeleteFileW(path.c_str()))) {
        setLastError(error);
    }
}
#endif

// 软链接目录
bool MCDevTool::createDirectoryJunction(const std::filesystem::path& target, const std::filesystem::path& link) {
    std::filesystem::create_directories(link.parent_path());
    // link 从第二次运行起通常是上一轮的 junction，只能摘链接，不能删进用户源目录。
    removeAllNoFollow(link);
    std::filesystem::create_directory(link);

    HANDLE h = CreateFileW(
        link.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr
    );

    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    std::wstring real       = std::filesystem::absolute(target).wstring();
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

    const auto totalLen = FIELD_OFFSET(REPARSE_DATA_BUFFER, PathBuffer) + substLen + sizeof(WCHAR)
                        +                           // substitute + null
                          printLen + sizeof(WCHAR); // print + null

    std::vector<char> buffer(totalLen);
    auto*             rp = reinterpret_cast<REPARSE_DATA_BUFFER*>(buffer.data());

    rp->ReparseTag           = IO_REPARSE_TAG_MOUNT_POINT;
    rp->ReparseDataLength    = WORD(totalLen - FIELD_OFFSET(REPARSE_DATA_BUFFER, SubstituteNameOffset));
    rp->Reserved             = 0;
    rp->SubstituteNameOffset = 0;
    rp->SubstituteNameLength = WORD(substLen);
    rp->PrintNameOffset      = WORD(substLen + sizeof(WCHAR));
    rp->PrintNameLength      = WORD(printLen);

    memcpy(rp->PathBuffer, substitute.c_str(), substLen);
    rp->PathBuffer[substitute.size()] = L'\0';
    memcpy((PBYTE)rp->PathBuffer + substLen + sizeof(WCHAR), real.c_str(), printLen);
    rp->PathBuffer[substitute.size() + real.size() + 1] = L'\0';

    DWORD bytesReturned;
    BOOL  ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, rp, totalLen, nullptr, 0, &bytesReturned, nullptr);

    CloseHandle(h);
    return ok == TRUE;
}

#else
bool MCDevTool::createDirectoryJunction(const std::filesystem::path&, const std::filesystem::path&) {
    return false;
}

bool MCDevTool::isLink(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_symlink(std::filesystem::symlink_status(path, error));
}

// POSIX 下标准保证 remove_all 不跟随符号链接。
void MCDevTool::removeAllNoFollow(const std::filesystem::path& path, std::error_code& error) noexcept {
    std::filesystem::remove_all(path, error);
}
#endif

void MCDevTool::removeAllNoFollow(const std::filesystem::path& path) {
    std::error_code error;
    removeAllNoFollow(path, error);
    if (error) {
        throw std::filesystem::filesystem_error("removeAllNoFollow", path, error);
    }
}

static void normalizeUUIDString(std::string& uuidStr) {
    uuidStr.erase(std::remove(uuidStr.begin(), uuidStr.end(), '-'), uuidStr.end());
}

namespace MCDevTool {
    static std::filesystem::path appDataCachePath;

    // 获取应用数据目录路径
    std::filesystem::path getAppDataPath() {
        if (appDataCachePath.empty()) {
            const char* appData = std::getenv("APPDATA");
            if (appData) {
                appDataCachePath = std::filesystem::path(appData);
            }
        }
        return appDataCachePath;
    }

    void setAppDataPathOverride(const std::filesystem::path& path) { appDataCachePath = path; }

    // 获取MinecraftPE_Netease数据目录
    std::filesystem::path getMinecraftDataPath() { return getAppDataPath() / "MinecraftPE_Netease"; }

    // 获取games/com.netease目录
    std::filesystem::path getGamesComNeteasePath() { return getMinecraftDataPath() / "games/com.netease"; }

    // 获取minecraftWorlds目录
    std::filesystem::path getMinecraftWorldsPath() { return getMinecraftDataPath() / "minecraftWorlds"; }

    // 获取行为包目录
    std::filesystem::path getBehaviorPacksPath() { return getGamesComNeteasePath() / "behavior_packs"; }

    // 获取资源包目录
    std::filesystem::path getResourcePacksPath() { return getGamesComNeteasePath() / "resource_packs"; }

    // 获取依赖包目录
    std::filesystem::path getDependenciesPacksPath() {
        // 依赖包并非官方目录 仅供开发工具使用
        return getGamesComNeteasePath() / "_dependencies_packs";
    }

    RuntimePackRoots getGlobalPackRoots() { return {getBehaviorPacksPath(), getResourcePacksPath()}; }

    // 清理运行时行为包目录（底下挂着指向源码的 junction）
    void cleanRuntimeBehaviorPacks() {
        auto runtimeBPPath = getBehaviorPacksPath();
        if (std::filesystem::is_directory(runtimeBPPath)) {
            removeAllNoFollow(runtimeBPPath);
        }
    }

    // 清理运行时资源包目录（底下挂着指向源码的 junction）
    void cleanRuntimeResourcePacks() {
        auto runtimeRPPath = getResourcePacksPath();
        if (std::filesystem::is_directory(runtimeRPPath)) {
            removeAllNoFollow(runtimeRPPath);
        }
    }

    // 同时清理双pack目录
    void cleanRuntimePacks() {
        cleanRuntimeBehaviorPacks();
        cleanRuntimeResourcePacks();
    }

    // 分析并link源代码addon目录到运行时行为/资源包目录
    Addon::PackInfo linkSourcePackToRuntimePack(const std::filesystem::path& sourceDir) {
        return linkSourcePackToRuntimePack(sourceDir, getGlobalPackRoots());
    }

    Addon::PackInfo linkSourcePackToRuntimePack(const std::filesystem::path& sourceDir, const RuntimePackRoots& roots) {
        auto info = Addon::parsePackInfo(sourceDir);
        if (!info) {
            return info;
        }
        if (info.type == Addon::PackType::BEHAVIOR) {
            auto uuid = info.uuid;
            normalizeUUIDString(uuid);
            auto destPath = roots.behavior / uuid;
            if (!createDirectoryJunction(sourceDir, destPath)) {
                std::cerr << "行为包软链接创建失败: " << Utils::pathToUtf8(sourceDir.filename()) << "\n";
            } else {
                info.path = destPath;
            }
        } else if (info.type == Addon::PackType::RESOURCE) {
            auto uuid = info.uuid;
            normalizeUUIDString(uuid);
            auto destPath = roots.resource / uuid;
            if (!createDirectoryJunction(sourceDir, destPath)) {
                std::cerr << "资源包软链接创建失败: " << Utils::pathToUtf8(sourceDir.filename()) << "\n";
            } else {
                info.path = destPath;
            }
        }
        return info;
    }

    // 分析并link源代码addon目录到运行时行为包目录 该版本支持资源+行为包组合
    std::vector<Addon::PackInfo> linkSourceAddonToRuntimePacks(const std::filesystem::path& sourceDir) {
        return linkSourceAddonToRuntimePacks(sourceDir, getGlobalPackRoots());
    }

    std::vector<Addon::PackInfo>
    linkSourceAddonToRuntimePacks(const std::filesystem::path& sourceDir, const RuntimePackRoots& roots) {
        std::vector<Addon::PackInfo> packs;
        // 先尝试处理当前目录为单一pack
        auto oncePackInfo = linkSourcePackToRuntimePack(sourceDir, roots);
        if (oncePackInfo) {
            packs.push_back(std::move(oncePackInfo));
            return packs;
        }
        // 遍历当前文件夹下的所有文件夹
        for (const auto& entry : std::filesystem::directory_iterator(sourceDir)) {
            if (entry.is_directory()) {
                auto packInfo = linkSourcePackToRuntimePack(entry.path(), roots);
                if (packInfo) {
                    packs.push_back(std::move(packInfo));
                }
            }
        }
        return packs;
    }
} // namespace MCDevTool
