#include <iostream>
#include <string>
#include <thread>
#include <functional>
#include <vector>
#include <filesystem>
#include <fstream>
#include <mcdevtool.h>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// 定义搜索Minecraft窗口PID的函数 标题包含 L"Minecraft"  返回PID
static int findMinecraftWindowPid() {
    struct Context {
        DWORD pid;
    } ctx{0};

    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL {
            auto& ctx = *(Context*)lParam;

            if (!IsWindowVisible(hwnd)) return TRUE;
            if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;

            // 标题过滤
            wchar_t buf[512];
            int     len = GetWindowTextW(hwnd, buf, 512);
            if (len == 0) return TRUE;

            std::wstring title = buf;
            if (title.find(L"Minecraft") == std::wstring::npos) return TRUE;

            // 获取PID
            DWORD winPid = 0;
            GetWindowThreadProcessId(hwnd, &winPid);
            ctx.pid = winPid;
            return FALSE; // 找到了就停止枚举
        },
        (LPARAM)&ctx
    );

    return static_cast<int>(ctx.pid); // 0表示未找到
}

int main(int argc, char* argv[]) {
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

    auto pid = findMinecraftWindowPid();
    if (pid == 0) {
        std::cerr << "Minecraft window not found." << std::endl;
        return 1;
    }
    auto data = MCDevTool::Style::captureMinecraftWindowJpeg(pid);
    if (!data.has_value()) {
        std::cerr << "Failed to capture Minecraft window: " << MCDevTool::Style::describeCaptureError(data.error())
                  << std::endl;
        return 1;
    }
    const std::filesystem::path outPut = argc > 1 ? argv[1] : "capture.jpg";
    std::ofstream               file(outPut, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Failed to open output file: " << outPut << std::endl;
        return 1;
    }
    file.write(reinterpret_cast<const char*>(data->data()), static_cast<std::streamsize>(data->size()));
    file.close();
    if (!file) {
        std::cerr << "Failed to write screenshot: " << outPut << std::endl;
        return 1;
    }
    std::cout << "Screenshot saved to: " << std::filesystem::absolute(outPut) << std::endl;

    return 0;
}
