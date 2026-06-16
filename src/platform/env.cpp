#include "env.h"
#include "mcdevtool/utils.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#endif

namespace MCDevTool::Platform {
    namespace {
        std::filesystem::path appDataCachePath;

#ifdef _WIN32
        std::optional<std::filesystem::path> gamePathCache;
        bool                                 hasNoGamePath = false;
#endif

#ifndef __APPLE__
        std::optional<std::filesystem::path> latestGameExePathCache;
#endif

#ifdef _WIN32
        std::vector<std::string> getAllDrives() {
            char  buffer[256];
            DWORD len = GetLogicalDriveStringsA(sizeof(buffer), buffer);

            std::vector<std::string> drives;
            char*                    p = buffer;
            while (*p) {
                drives.emplace_back(p);
                p += std::strlen(p) + 1;
            }
            return drives;
        }
#endif
    } // namespace

    std::filesystem::path getAppDataPath() {
        if (appDataCachePath.empty()) {
#ifdef _WIN32
            const char* appData = std::getenv("APPDATA");
            if (appData) {
                appDataCachePath = std::filesystem::path(appData);
            }
#else
            const char* xdgDataHome = std::getenv("XDG_DATA_HOME");
            if (xdgDataHome && *xdgDataHome) {
                appDataCachePath = std::filesystem::path(xdgDataHome);
            } else if (const char* home = std::getenv("HOME")) {
#ifdef __APPLE__
                appDataCachePath = std::filesystem::path(home) / "Library/Application Support";
#else
                appDataCachePath = std::filesystem::path(home) / ".local/share";
#endif
            }
#endif
        }
        return appDataCachePath;
    }

    bool createPackDirectoryLink(const std::filesystem::path& target, const std::filesystem::path& link) {
#ifdef _WIN32
        std::filesystem::create_directories(link.parent_path());
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

        struct ReparseDataBuffer {
            DWORD ReparseTag;
            WORD  ReparseDataLength;
            WORD  Reserved;
            WORD  SubstituteNameOffset;
            WORD  SubstituteNameLength;
            WORD  PrintNameOffset;
            WORD  PrintNameLength;
            WCHAR PathBuffer[1];
        };

        const auto substLen = substitute.size() * sizeof(WCHAR);
        const auto printLen = real.size() * sizeof(WCHAR);

        const auto totalLen = FIELD_OFFSET(ReparseDataBuffer, PathBuffer) + substLen + sizeof(WCHAR)
                            + printLen + sizeof(WCHAR);

        std::vector<char> buffer(totalLen);
        auto*             rp = reinterpret_cast<ReparseDataBuffer*>(buffer.data());

        rp->ReparseTag           = IO_REPARSE_TAG_MOUNT_POINT;
        rp->ReparseDataLength    = WORD(totalLen - FIELD_OFFSET(ReparseDataBuffer, SubstituteNameOffset));
        rp->Reserved             = 0;
        rp->SubstituteNameOffset = 0;
        rp->SubstituteNameLength = WORD(substLen);
        rp->PrintNameOffset      = WORD(substLen + sizeof(WCHAR));
        rp->PrintNameLength      = WORD(printLen);

        std::memcpy(rp->PathBuffer, substitute.c_str(), substLen);
        rp->PathBuffer[substitute.size()] = L'\0';
        std::memcpy(reinterpret_cast<PBYTE>(rp->PathBuffer) + substLen + sizeof(WCHAR), real.c_str(), printLen);
        rp->PathBuffer[substitute.size() + real.size() + 1] = L'\0';

        DWORD bytesReturned;
        BOOL  ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, rp, totalLen, nullptr, 0, &bytesReturned, nullptr);

        CloseHandle(h);
        return ok == TRUE;
#else
        std::error_code ec;
        std::filesystem::create_directories(link.parent_path(), ec);
        if (ec) {
            return false;
        }

        if (std::filesystem::exists(link, ec) || std::filesystem::is_symlink(link, ec)) {
            std::filesystem::remove_all(link, ec);
            if (ec) {
                return false;
            }
        }

        auto absoluteTarget = std::filesystem::absolute(target, ec);
        if (ec) {
            return false;
        }
        std::filesystem::create_directory_symlink(absoluteTarget, link, ec);
        return !ec;
#endif
    }

    std::optional<std::filesystem::path> autoSearchMCStudioDownloadGamePath() {
#ifdef _WIN32
        if (gamePathCache || hasNoGamePath) {
            return gamePathCache;
        }
        auto drives = getAllDrives();
        for (const auto& drive : drives) {
            std::filesystem::path path = drive + "MCStudioDownload/game/MinecraftPE_Netease";
            if (std::filesystem::is_directory(path)) {
                for (const auto& entry : std::filesystem::directory_iterator(path)) {
                    if (entry.is_directory()) {
                        auto exePath = entry.path() / "Minecraft.Windows.exe";
                        if (std::filesystem::is_regular_file(exePath)) {
                            gamePathCache = path;
                            return path;
                        }
                    }
                }
            }
        }
        hasNoGamePath = true;
        return std::nullopt;
#else
        // macOS and other non-Windows platforms do not have a reliable MCStudioDownload signature.
        return std::nullopt;
#endif
    }

    std::optional<std::filesystem::path> autoMatchLatestGameExePath() {
#ifdef __APPLE__
        // macOS must use an explicitly configured executable path.
        return std::nullopt;
#else
        if (latestGameExePathCache) {
            return latestGameExePathCache;
        }
        auto gamePath = autoSearchMCStudioDownloadGamePath();
        if (!gamePath) {
            return std::nullopt;
        }

        std::optional<std::filesystem::path> latestExePath;
        MCDevTool::Utils::Version            latestVersion;
        uint64_t                             latestVersionNum = 0;
        for (const auto& entry : std::filesystem::directory_iterator(gamePath.value())) {
            if (entry.is_directory()) {
                std::string folderName = Utils::pathToUtf8(entry.path().filename());
                MCDevTool::Utils::Version ver{folderName};
                if (!ver) {
                    continue;
                }
                auto exePath = entry.path() / "Minecraft.Windows.exe";
                if (std::filesystem::is_regular_file(exePath)) {
                    if (latestVersionNum == 0 || latestVersion < ver) {
                        latestVersion = std::move(ver);
                        latestExePath = exePath;
                        latestVersionNum++;
                    }
                }
            }
        }
        if (latestExePath) {
            latestGameExePathCache = latestExePath;
        }
        return latestExePath;
#endif
    }
} // namespace MCDevTool::Platform
