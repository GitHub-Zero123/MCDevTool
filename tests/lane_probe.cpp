// Lane 注入测试用的探针：把自己取到的 Roaming 和 Local AppData 写进参数给的文件。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <cwchar>

namespace {
    // 直接写 UTF-16 原始字节，避免中文路径经过 CRT 区域设置转换。
    void writeFolder(HANDLE file, REFKNOWNFOLDERID id) {
        PWSTR path = nullptr;
        const wchar_t* text = SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path)) ? path : L"<failed>";
        DWORD written = 0;
        WriteFile(file, text, static_cast<DWORD>(wcslen(text) * sizeof(wchar_t)), &written, nullptr);
        WriteFile(file, L"\n", sizeof(wchar_t), &written, nullptr);
        CoTaskMemFree(path);
    }
} // namespace

int wmain(int argc, wchar_t* argv[]) {
    if (argc < 2) return 2;
    HANDLE file = CreateFileW(argv[1], GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 3;
    writeFolder(file, FOLDERID_RoamingAppData);
    writeFolder(file, FOLDERID_LocalAppData);
    CloseHandle(file);
    return 0;
}
