#include "mcdevtool/env.h"
#include "mcdevtool/utils.h"
#include <algorithm>
#include <cstdlib>
#include <utility> // std::move
#include <iostream>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>

// 软链接目录
bool MCDevTool::createDirectoryJunction(const std::filesystem::path& target, const std::filesystem::path& link) {
    // 默认以WIN32 MSVC STL为标准
    // 其他实现可能不遵守 MSVC 的 remove_all 语义，误判 junction 为普通目录而递归删除源目录内容。
    std::filesystem::create_directories(link.parent_path());
    // link 从第二次运行起通常已经是上一轮留下的 junction，而 target 是用户真实的源目录。
    // 这里的 remove_all 只摘掉链接本身，不会递归进去删掉 target 的内容：MSVC 的 remove_all
    // 先直接调 RemoveDirectoryW，只有收到 ERROR_DIRECTORY_NOT_EMPTY 才递归；而
    // RemoveDirectoryW 对 junction 的语义就是"无论目标是否为空都只删链接"，第一步即成功，
    // 根本走不到递归分支。
    //
    // 这是实现定义行为，不是标准保证——标准只承诺不跟随 symlink，而 junction 在标准模型里
    // 并不是 symlink。换到 MSVC STL 以外的实现（例如 MinGW 的 libstdc++）必须重新验证，
    // 误判的后果是删掉用户的源目录。
    //
    // 另外：MSVC 上 is_symlink(junction) 返回 false（junction 是独立的 file_type::junction），
    // 不要拿 is_symlink 当"是不是链接"的防护，那样写等于没写。
    if (std::filesystem::exists(link)) {
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
#endif

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

    // 清理运行时行为包目录
    // 这里删的是父目录，底下通常挂着 linkSourcePackToRuntimePack 建的 junction，所以会真正
    // 走进 remove_all 的递归分支。依靠的是另一层保障：MSVC 把 junction 归为
    // file_type::junction 而非 directory，递归条件不成立，于是只解除链接、不碰源目录内容。
    // 同样是实现定义行为，完整依据见 createDirectoryJunction 处的说明。
    void cleanRuntimeBehaviorPacks() {
        auto runtimeBPPath = getBehaviorPacksPath();
        if (std::filesystem::is_directory(runtimeBPPath)) {
            std::filesystem::remove_all(runtimeBPPath);
        }
    }

    // 清理运行时资源包目录
    // 与 cleanRuntimeBehaviorPacks 同理：底下挂着 junction，靠 file_type::junction 的归类
    // 保证只解除链接。
    void cleanRuntimeResourcePacks() {
        auto runtimeRPPath = getResourcePacksPath();
        if (std::filesystem::is_directory(runtimeRPPath)) {
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
        if (!info) {
            return info;
        }
        if (info.type == Addon::PackType::BEHAVIOR) {
            auto uuid = info.uuid;
            normalizeUUIDString(uuid);
            auto destPath = getBehaviorPacksPath() / uuid;
            if (!createDirectoryJunction(sourceDir, destPath)) {
                std::cerr << "行为包软链接创建失败: " << Utils::pathToUtf8(sourceDir.filename()) << "\n";
            } else {
                info.path = destPath;
            }
        } else if (info.type == Addon::PackType::RESOURCE) {
            auto uuid = info.uuid;
            normalizeUUIDString(uuid);
            auto destPath = getResourcePacksPath() / uuid;
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
        std::vector<Addon::PackInfo> packs;
        // 先尝试处理当前目录为单一pack
        auto oncePackInfo = linkSourcePackToRuntimePack(sourceDir);
        if (oncePackInfo) {
            packs.push_back(std::move(oncePackInfo));
            return packs;
        }
        // 遍历当前文件夹下的所有文件夹
        for (const auto& entry : std::filesystem::directory_iterator(sourceDir)) {
            if (entry.is_directory()) {
                auto packInfo = linkSourcePackToRuntimePack(entry.path());
                if (packInfo) {
                    packs.push_back(std::move(packInfo));
                }
            }
        }
        return packs;
    }
} // namespace MCDevTool
