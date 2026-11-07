// mcdev-lane-shim：mcdk 在游戏入口点之前注入，让游戏取到的 Roaming AppData 指向 Lane 目录。
// 任何一步做不到都直接结束游戏，不允许“以为隔离了其实没有”。
#include "mcdev_lane_shim.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <knownfolders.h>
#include <objbase.h>
#include <shlobj.h>

#include <MinHook.h>

#include <intrin.h>

#include <atomic>
#include <cstring>
#include <cwchar>
#include <string>

namespace {
    using KnownFolderPathFn = HRESULT(WINAPI*)(REFKNOWNFOLDERID, DWORD, HANDLE, PWSTR*);

    constexpr wchar_t StorageModule[] = L"windows.storage.dll";

    std::wstring      gRoot;
    KnownFolderPathFn gOriginal = nullptr;
    std::atomic<bool> gHooked{false};
    PVOID             gNotificationCookie = nullptr;

    [[noreturn]] void failClosed(UINT exitCode) {
        TerminateProcess(GetCurrentProcess(), exitCode);
        for (;;) Sleep(INFINITE);
    }

    // 排查用：设置 MCDEV_LANE_TRACE 为文件路径时，记录每次取 AppData 的调用。
    std::wstring gTrace;

    void trace(const wchar_t* api, int folder, void* caller) {
        if (gTrace.empty()) return;
        HMODULE module = nullptr;
        wchar_t name[MAX_PATH] = L"?";
        if (GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                static_cast<LPCWSTR>(caller),
                &module
            )) {
            GetModuleFileNameW(module, name, MAX_PATH);
        }
        wchar_t   line[MAX_PATH + 96];
        const int length = swprintf_s(line, L"%ls folder=0x%x caller=%ls\r\n", api, folder, name);
        HANDLE    file   = CreateFileW(
            gTrace.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr
        );
        if (file != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(file, line, static_cast<DWORD>(length * sizeof(wchar_t)), &written, nullptr);
            CloseHandle(file);
        }
    }

    bool isRoamingCsidl(int csidl) { return (csidl & 0xFF) == CSIDL_APPDATA; }

    HRESULT WINAPI redirectedKnownFolderPath(REFKNOWNFOLDERID id, DWORD flags, HANDLE token, PWSTR* path) {
        if (!IsEqualGUID(id, FOLDERID_RoamingAppData)) {
            return gOriginal(id, flags, token, path);
        }
        trace(L"SHGetKnownFolderPath", 0, _ReturnAddress());
        const auto bytes = (gRoot.size() + 1) * sizeof(wchar_t);
        *path            = static_cast<PWSTR>(CoTaskMemAlloc(bytes));
        if (*path == nullptr) return E_OUTOFMEMORY;
        std::memcpy(*path, gRoot.c_str(), bytes);
        return S_OK;
    }

    // 老式接口不经过 SHGetKnownFolderPath 的导出，要单独接住。
    using FolderPathWFn  = HRESULT(WINAPI*)(HWND, int, HANDLE, DWORD, LPWSTR);
    using FolderPathAFn  = HRESULT(WINAPI*)(HWND, int, HANDLE, DWORD, LPSTR);
    using SpecialPathWFn = BOOL(WINAPI*)(HWND, LPWSTR, int, BOOL);
    using SpecialPathAFn = BOOL(WINAPI*)(HWND, LPSTR, int, BOOL);
    FolderPathWFn  gFolderPathW  = nullptr;
    FolderPathAFn  gFolderPathA  = nullptr;
    SpecialPathWFn gSpecialPathW = nullptr;
    SpecialPathAFn gSpecialPathA = nullptr;

    bool copyRoot(LPWSTR out) { return wcscpy_s(out, MAX_PATH, gRoot.c_str()) == 0; }

    bool copyRoot(LPSTR out) {
        return WideCharToMultiByte(CP_ACP, 0, gRoot.c_str(), -1, out, MAX_PATH, nullptr, nullptr) > 0;
    }

    HRESULT WINAPI redirectedFolderPathW(HWND window, int csidl, HANDLE token, DWORD flags, LPWSTR path) {
        if (!isRoamingCsidl(csidl)) return gFolderPathW(window, csidl, token, flags, path);
        trace(L"SHGetFolderPathW", csidl, _ReturnAddress());
        return copyRoot(path) ? S_OK : E_FAIL;
    }

    HRESULT WINAPI redirectedFolderPathA(HWND window, int csidl, HANDLE token, DWORD flags, LPSTR path) {
        if (!isRoamingCsidl(csidl)) return gFolderPathA(window, csidl, token, flags, path);
        trace(L"SHGetFolderPathA", csidl, _ReturnAddress());
        return copyRoot(path) ? S_OK : E_FAIL;
    }

    BOOL WINAPI redirectedSpecialPathW(HWND window, LPWSTR path, int csidl, BOOL create) {
        if (!isRoamingCsidl(csidl)) return gSpecialPathW(window, path, csidl, create);
        trace(L"SHGetSpecialFolderPathW", csidl, _ReturnAddress());
        return copyRoot(path) ? TRUE : FALSE;
    }

    BOOL WINAPI redirectedSpecialPathA(HWND window, LPSTR path, int csidl, BOOL create) {
        if (!isRoamingCsidl(csidl)) return gSpecialPathA(window, path, csidl, create);
        trace(L"SHGetSpecialFolderPathA", csidl, _ReturnAddress());
        return copyRoot(path) ? TRUE : FALSE;
    }

    // 函数不存在就跳过；存在却钩不上视为失败。
    bool hookIfPresent(const wchar_t* module, const char* name, LPVOID detour, LPVOID* original) {
        HMODULE handle = GetModuleHandleW(module);
        if (handle == nullptr || GetProcAddress(handle, name) == nullptr) return true;
        return MH_CreateHookApi(module, name, detour, original) == MH_OK;
    }

    // shell32 的 SHGetKnownFolderPath 只是跳到 windows.storage，钩这一处就覆盖两条入口。
    bool hookStorage() {
        if (gHooked.exchange(true)) return true;
        return MH_CreateHookApi(
                   StorageModule,
                   "SHGetKnownFolderPath",
                   reinterpret_cast<LPVOID>(&redirectedKnownFolderPath),
                   reinterpret_cast<LPVOID*>(&gOriginal)
               ) == MH_OK
            && hookIfPresent(
                StorageModule,
                "SHGetFolderPathW",
                reinterpret_cast<LPVOID>(&redirectedFolderPathW),
                reinterpret_cast<LPVOID*>(&gFolderPathW)
            )
            && hookIfPresent(
                StorageModule,
                "SHGetFolderPathA",
                reinterpret_cast<LPVOID>(&redirectedFolderPathA),
                reinterpret_cast<LPVOID*>(&gFolderPathA)
            )
            && hookIfPresent(
                L"shell32.dll",
                "SHGetSpecialFolderPathW",
                reinterpret_cast<LPVOID>(&redirectedSpecialPathW),
                reinterpret_cast<LPVOID*>(&gSpecialPathW)
            )
            && hookIfPresent(
                L"shell32.dll",
                "SHGetSpecialFolderPathA",
                reinterpret_cast<LPVOID>(&redirectedSpecialPathA),
                reinterpret_cast<LPVOID*>(&gSpecialPathA)
            )
            && MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
    }

    struct UnicodeString {
        USHORT Length;
        USHORT MaximumLength;
        PWSTR  Buffer;
    };

    struct DllLoadedData {
        ULONG                Flags;
        const UnicodeString* FullDllName;
        const UnicodeString* BaseDllName;
        PVOID                DllBase;
        ULONG                SizeOfImage;
    };

    using DllNotificationFn     = VOID(CALLBACK*)(ULONG, const DllLoadedData*, PVOID);
    using RegisterNotificationFn = LONG(NTAPI*)(ULONG, DllNotificationFn, PVOID, PVOID*);
    constexpr ULONG DllLoaded   = 1;

    bool isStorageModule(const UnicodeString* name) {
        constexpr auto length = sizeof(StorageModule) / sizeof(wchar_t) - 1;
        return name != nullptr && name->Length / sizeof(wchar_t) == length
            && _wcsnicmp(name->Buffer, StorageModule, length) == 0;
    }

    // windows.storage 由 shell32 延迟加载。它载入的那一刻游戏还来不及调用，加壳程序也还没复制函数开头。
    VOID CALLBACK onDllNotification(ULONG reason, const DllLoadedData* data, PVOID) {
        if (reason == DllLoaded && data != nullptr && isStorageModule(data->BaseDllName) && !hookStorage()) {
            failClosed(MCDEV_LANE_EXIT_HOOK_FAILED);
        }
    }

    bool waitForStorage() {
        const auto registerNotification = reinterpret_cast<RegisterNotificationFn>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification")
        );
        return registerNotification != nullptr
            && registerNotification(0, &onDllNotification, nullptr, &gNotificationCookie) >= 0;
    }

    void signalArmed() {
        wchar_t name[64];
        swprintf_s(name, MCDEV_LANE_ARMED_EVENT_FORMAT, GetCurrentProcessId());
        if (HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, name)) {
            SetEvent(event);
            CloseHandle(event);
        }
    }

    void arm() {
        const DWORD length = GetEnvironmentVariableW(MCDEV_LANE_ROOT_ENV, nullptr, 0);
        if (length <= 1) failClosed(MCDEV_LANE_EXIT_SETUP_FAILED);
        gRoot.resize(length);
        gRoot.resize(GetEnvironmentVariableW(MCDEV_LANE_ROOT_ENV, gRoot.data(), length));
        if (const DWORD traceLength = GetEnvironmentVariableW(L"MCDEV_LANE_TRACE", nullptr, 0); traceLength > 1) {
            gTrace.resize(traceLength);
            gTrace.resize(GetEnvironmentVariableW(L"MCDEV_LANE_TRACE", gTrace.data(), traceLength));
        }

        if (MH_Initialize() != MH_OK) failClosed(MCDEV_LANE_EXIT_SETUP_FAILED);
        const bool armed = GetModuleHandleW(StorageModule) != nullptr ? hookStorage() : waitForStorage();
        if (!armed) failClosed(MCDEV_LANE_EXIT_HOOK_FAILED);
        signalArmed();
    }
} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        arm();
    }
    return TRUE;
}
