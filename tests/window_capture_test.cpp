#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <gl/GL.h>
#include <wincodec.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <future>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include "mcdevtool/style.h"

namespace {
    int printMessages = 0;

    void require(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_PRINT || message == WM_PRINTCLIENT) {
            ++printMessages;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    struct OpenGlWindow {
        HWND  hwnd = nullptr;
        HDC   dc   = nullptr;
        HGLRC gl   = nullptr;

        void create() {
            WNDCLASSW cls{};
            cls.style         = CS_OWNDC;
            cls.lpfnWndProc   = windowProc;
            cls.hInstance     = GetModuleHandleW(nullptr);
            cls.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
            cls.lpszClassName = L"MCDevToolWgcTest";
            require(RegisterClassW(&cls) != 0, "RegisterClass failed");
            hwnd = CreateWindowExW(
                0,
                cls.lpszClassName,
                L"Minecraft WGC regression test",
                WS_OVERLAPPEDWINDOW,
                100,
                100,
                800,
                700,
                nullptr,
                nullptr,
                cls.hInstance,
                nullptr
            );
            require(hwnd != nullptr, "CreateWindow failed");
            dc = GetDC(hwnd);
            PIXELFORMATDESCRIPTOR format{};
            format.nSize      = sizeof(format);
            format.nVersion   = 1;
            format.dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
            format.iPixelType = PFD_TYPE_RGBA;
            format.cColorBits = 32;
            const int index   = ChoosePixelFormat(dc, &format);
            require(index != 0 && SetPixelFormat(dc, index, &format), "SetPixelFormat failed");
            gl = wglCreateContext(dc);
            require(gl != nullptr && wglMakeCurrent(dc, gl), "OpenGL context failed");
            std::cout << "GL renderer: " << glGetString(GL_RENDERER) << '\n';
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            resize(800, 600);
        }

        void resize(int width, int height) {
            RECT rect{0, 0, width, height};
            require(
                AdjustWindowRectEx(&rect, static_cast<DWORD>(GetWindowLongW(hwnd, GWL_STYLE)), FALSE, 0),
                "AdjustWindowRectEx failed"
            );
            require(
                SetWindowPos(
                    hwnd,
                    nullptr,
                    0,
                    0,
                    rect.right - rect.left,
                    rect.bottom - rect.top,
                    SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED
                ),
                "Resize failed"
            );
        }

        void render() {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            RECT rect{};
            GetClientRect(hwnd, &rect);
            require(wglMakeCurrent(dc, gl), "Make OpenGL context current failed");
            glViewport(0, 0, rect.right, rect.bottom);
            glEnable(GL_SCISSOR_TEST);
            // 上半红色、下半蓝色：同时检查方向、裁剪、颜色和白屏回归。
            glScissor(0, 0, rect.right, rect.bottom / 2);
            glClearColor(0.1f, 0.2f, 0.8f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glScissor(0, rect.bottom / 2, rect.right, rect.bottom - rect.bottom / 2);
            glClearColor(0.8f, 0.2f, 0.1f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);
            require(glGetError() == GL_NO_ERROR, "OpenGL rendering error");
            SwapBuffers(dc);
        }

        ~OpenGlWindow() {
            wglMakeCurrent(nullptr, nullptr);
            if (gl) {
                wglDeleteContext(gl);
            }
            if (dc) {
                ReleaseDC(hwnd, dc);
            }
            if (hwnd) {
                DestroyWindow(hwnd);
            }
            UnregisterClassW(L"MCDevToolWgcTest", GetModuleHandleW(nullptr));
        }
    };

    struct Occluder {
        HWND hwnd = nullptr;
        ~Occluder() {
            if (hwnd) {
                DestroyWindow(hwnd);
            }
        }
    };

    void decodeJpegSize(const std::vector<uint8_t>& jpeg, UINT& width, UINT& height) {
        auto                    factory = winrt::create_instance<IWICImagingFactory>(CLSID_WICImagingFactory);
        winrt::com_ptr<IStream> stream;
        winrt::check_hresult(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()));
        winrt::check_hresult(stream->Write(jpeg.data(), static_cast<ULONG>(jpeg.size()), nullptr));
        winrt::check_hresult(stream->Seek({}, STREAM_SEEK_SET, nullptr));
        winrt::com_ptr<IWICBitmapDecoder> decoder;
        winrt::check_hresult(
            factory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.put())
        );
        winrt::com_ptr<IWICBitmapFrameDecode> frame;
        winrt::check_hresult(decoder->GetFrame(0, frame.put()));
        winrt::check_hresult(frame->GetSize(&width, &height));
    }

    // 区域截图校验：尺寸必须精确，且整块都是期望的单色。测试窗口上半红、下半蓝，
    // 因此这同时验证了区域的偏移和方向没有搞反。
    void verifySolidRegion(const std::vector<uint8_t>& jpeg, UINT expectedWidth, UINT expectedHeight, bool expectRed) {
        auto                    factory = winrt::create_instance<IWICImagingFactory>(CLSID_WICImagingFactory);
        winrt::com_ptr<IStream> stream;
        winrt::check_hresult(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()));
        winrt::check_hresult(stream->Write(jpeg.data(), static_cast<ULONG>(jpeg.size()), nullptr));
        winrt::check_hresult(stream->Seek({}, STREAM_SEEK_SET, nullptr));
        winrt::com_ptr<IWICBitmapDecoder> decoder;
        winrt::check_hresult(
            factory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.put())
        );
        winrt::com_ptr<IWICBitmapFrameDecode> frame;
        winrt::check_hresult(decoder->GetFrame(0, frame.put()));
        UINT width = 0, height = 0;
        winrt::check_hresult(frame->GetSize(&width, &height));
        require(width == expectedWidth && height == expectedHeight, "Incorrect region size");
        winrt::com_ptr<IWICFormatConverter> converter;
        winrt::check_hresult(factory->CreateFormatConverter(converter.put()));
        winrt::check_hresult(converter->Initialize(
            frame.get(),
            GUID_WICPixelFormat24bppRGB,
            WICBitmapDitherTypeNone,
            nullptr,
            0,
            WICBitmapPaletteTypeCustom
        ));
        for (int x : {8, static_cast<int>(width) - 9}) {
            for (int y : {8, static_cast<int>(height) - 9}) {
                WICRect             sample{x, y, 1, 1};
                std::array<BYTE, 3> pixel{};
                winrt::check_hresult(converter->CopyPixels(&sample, 3, 3, pixel.data()));
                const bool matches = expectRed ? (pixel[0] > 170 && pixel[2] < 70) : (pixel[2] > 170 && pixel[0] < 70);
                require(matches, "Region captured the wrong part of the client area");
            }
        }
    }

    void verifyJpeg(const std::vector<uint8_t>& jpeg, UINT expectedWidth, UINT expectedHeight) {
        auto                    factory = winrt::create_instance<IWICImagingFactory>(CLSID_WICImagingFactory);
        winrt::com_ptr<IStream> stream;
        winrt::check_hresult(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()));
        winrt::check_hresult(stream->Write(jpeg.data(), static_cast<ULONG>(jpeg.size()), nullptr));
        winrt::check_hresult(stream->Seek({}, STREAM_SEEK_SET, nullptr));
        winrt::com_ptr<IWICBitmapDecoder> decoder;
        winrt::check_hresult(
            factory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.put())
        );
        winrt::com_ptr<IWICBitmapFrameDecode> frame;
        winrt::check_hresult(decoder->GetFrame(0, frame.put()));
        UINT width = 0, height = 0;
        winrt::check_hresult(frame->GetSize(&width, &height));
        require(width == expectedWidth && height == expectedHeight, "Incorrect JPEG dimensions / client crop");
        winrt::com_ptr<IWICFormatConverter> converter;
        winrt::check_hresult(factory->CreateFormatConverter(converter.put()));
        winrt::check_hresult(converter->Initialize(
            frame.get(),
            GUID_WICPixelFormat24bppRGB,
            WICBitmapDitherTypeNone,
            nullptr,
            0,
            WICBitmapPaletteTypeCustom
        ));
        // 靠近四角取样，同时避开 Windows 11 的圆角像素。
        for (int x : {16, static_cast<int>(width) - 17}) {
            for (int y : {16, static_cast<int>(height) - 17}) {
                WICRect             sample{x, y, 1, 1};
                std::array<BYTE, 3> pixel{};
                winrt::check_hresult(converter->CopyPixels(&sample, 3, 3, pixel.data()));
                const bool top = y < static_cast<int>(height / 2);
                require(
                    pixel[top ? 0 : 2] > 170 && pixel[top ? 2 : 0] < 70 && pixel[1] < 90,
                    "Wrong OpenGL pixels (white/occluded/inverted/incorrect client crop)"
                );
            }
        }
    }

    void captureAndVerify(
        OpenGlWindow& window,
        UINT          width,
        UINT          height,
        const char*   label,
        bool          resizeDuringCapture = false
    ) {
        // 从已初始化 STA 的线程调用，验证内部 MTA 隔离且无需调用者消息泵。
        auto future = std::async(std::launch::async, [] {
            winrt::init_apartment(winrt::apartment_type::single_threaded);
            auto result = MCDevTool::Style::captureMinecraftWindowJpeg(static_cast<int>(GetCurrentProcessId()));
            winrt::uninit_apartment();
            return result;
        });
        if (resizeDuringCapture) {
            window.resize(640, 360);
        }
        while (future.wait_for(std::chrono::milliseconds(16)) != std::future_status::ready) {
            window.render();
        }
        auto result = future.get();
        require(result.has_value() && !result->empty(), "WGC returned no image");
        try {
            verifyJpeg(*result, width, height);
        } catch (...) {
            std::ofstream file("window_capture_failure.jpg", std::ios::binary);
            file.write(reinterpret_cast<const char*>(result->data()), static_cast<std::streamsize>(result->size()));
            throw;
        }
        require(printMessages == 0, "Capture sent WM_PRINT / WM_PRINTCLIENT to the OpenGL window");
        std::cout << "PASS: " << label << '\n';
    }

    // 持续抖动窗口尺寸下截图。覆盖帧池按内容尺寸重建的分支，以及窗口几何与到手的帧不断
    // 错位的情况——旧实现在后者上只会空转到取帧超时然后返回 nullopt。
    //
    // 注意：能否走到按比例换算的裁剪兜底取决于时序，并不确定。实测同一台机器上三次分别
    // 是 144ms / 286ms / 756ms，只有超过严格窗口的那次才进了兜底。要确定性覆盖兜底需要
    // 在生产代码里开测试钩子，为那十行算术不值得，因此这里只断言"仍然交得出一张正常的
    // 图"，并把耗时打出来供人判断实际走了哪条路。
    void captureDuringContinuousResize(OpenGlWindow& window) {
        auto future = std::async(std::launch::async, [] {
            return MCDevTool::Style::captureMinecraftWindowJpeg(static_cast<int>(GetCurrentProcessId()));
        });
        const auto start = std::chrono::steady_clock::now();
        int        step  = 0;
        while (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
            if (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(900)) {
                window.resize(600 + (step % 7) * 8, 400 + (step % 5) * 8);
                ++step;
            }
            window.render();
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        const auto result  = future.get();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - start
        )
                                 .count();
        require(result.has_value() && !result->empty(), "Capture under continuous resize produced no image");
        require(step > 0, "Window was never resized; the fallback path was not exercised");

        // 兜底路径是按比例换算的裁剪，尺寸不必与某一瞬间的客户区严格相等；要求的是仍然
        // 交出一张能解码、比例和上限都合理的图，而不是一无所获。
        UINT width = 0, height = 0;
        decodeJpegSize(*result, width, height);
        require(width > 0 && height > 0 && height <= 480, "Capture under continuous resize produced an implausible size");
        std::cout << "PASS: continuous resize / frame pool recreate (" << width << 'x' << height << ", " << elapsed
                  << "ms)\n";
    }

    // 区域与 maxHeight 的行为。客户区固定 800x600：上半红、下半蓝。
    void captureRegionsAndLimits(OpenGlWindow& window) {
        using namespace MCDevTool::Style;
        window.resize(800, 600);
        for (int i = 0; i < 4; ++i) {
            window.render();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        const int pid = static_cast<int>(GetCurrentProcessId());

        // 上半：800x300，未超过 480 上限所以不缩放，且必须全红。
        auto top = captureMinecraftWindowJpeg(pid, {.region = {0.0, 0.0, 1.0, 0.5}});
        require(top.has_value(), "Top-half region capture failed");
        verifySolidRegion(*top, 800, 300, true);
        std::cout << "PASS: region top half (800x300, red)\n";

        // 下半：同样尺寸但必须全蓝，能抓出区域上下颠倒或偏移的错误。
        auto bottom = captureMinecraftWindowJpeg(pid, {.region = {0.0, 0.5, 1.0, 1.0}});
        require(bottom.has_value(), "Bottom-half region capture failed");
        verifySolidRegion(*bottom, 800, 300, false);
        std::cout << "PASS: region bottom half (800x300, blue)\n";

        // maxHeight 生效：800x600 客户区限到 240 高，宽按比例变 320。
        auto limited = captureMinecraftWindowJpeg(pid, {.maxHeight = 240});
        require(limited.has_value(), "maxHeight capture failed");
        verifyJpeg(*limited, 320, 240);
        std::cout << "PASS: max_height 240 (320x240)\n";

        // 坏区域必须立刻报错，而不是开一次捕获再失败。
        const auto inverted = captureMinecraftWindowJpeg(pid, {.region = {0.8, 0.0, 0.2, 1.0}});
        require(
            !inverted.has_value() && inverted.error() == CaptureError::InvalidRegion,
            "Inverted region must report InvalidRegion"
        );
        const auto outOfRange = captureMinecraftWindowJpeg(pid, {.region = {0.0, 0.0, 1.5, 1.0}});
        require(
            !outOfRange.has_value() && outOfRange.error() == CaptureError::InvalidRegion,
            "Out-of-range region must report InvalidRegion"
        );
        std::cout << "PASS: invalid region rejected\n";
    }

    // 按当前客户区推算截图应有的尺寸，跟 encodeJpeg 的缩放规则一致。标题栏高度随
    // DPI 和主题变化，不能写死。
    void expectedJpegSize(HWND hwnd, UINT& width, UINT& height, unsigned maxHeight = 480) {
        RECT client{};
        require(GetClientRect(hwnd, &client) != FALSE, "GetClientRect failed");
        const UINT clientWidth  = static_cast<UINT>(client.right);
        const UINT clientHeight = static_cast<UINT>(client.bottom);
        require(clientWidth > 0 && clientHeight > 0, "Empty client area");
        height = std::min(clientHeight, maxHeight);
        width  = std::max(
            1u,
            static_cast<UINT>((static_cast<uint64_t>(clientWidth) * height + clientHeight / 2) / clientHeight)
        );
    }

    // 运行时摘掉标题栏。项目的样式功能走的是 SWP_NOSIZE | SWP_FRAMECHANGED：窗口外框
    // 尺寸一点没变，只有客户区变大——而裁剪的几何严格比对比的正是外框尺寸，这种变化它
    // 发现不了。一旦客户区坐标和到手的帧不同步，截出来就是"顶上还留着标题栏、底下被切掉"。
    // 这里直接调项目自己的 applyStyleToMinecraftWindow，而不是在测试里重写一遍隐藏逻辑。
    void captureAfterHidingTitleBar(OpenGlWindow& window) {
        using namespace MCDevTool::Style;
        const int pid = static_cast<int>(GetCurrentProcessId());

        // 先确保是一个带标题栏的普通窗口。
        SetWindowLongW(window.hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPos(
            window.hwnd,
            nullptr,
            0,
            0,
            0,
            0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED
        );
        window.resize(800, 600);
        for (int i = 0; i < 4; ++i) {
            window.render();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }

        RECT before{};
        require(GetClientRect(window.hwnd, &before) != FALSE, "GetClientRect failed");

        require(applyStyleToMinecraftWindow(pid, StyleConfig{.hideTitleBar = true}), "Applying window style failed");

        RECT after{};
        require(GetClientRect(window.hwnd, &after) != FALSE, "GetClientRect failed");
        require(after.bottom > before.bottom, "Hiding the title bar should have grown the client area");

        // 不给任何缓冲时间，紧接着就截图。尺寸必须对上新的客户区，而且上半仍是红、下半
        // 仍是蓝——裁到旧帧的话顶部取到的会是标题栏而不是红色。
        UINT width = 0, height = 0;
        expectedJpegSize(window.hwnd, width, height);
        captureAndVerify(window, width, height, "title bar hidden at runtime (client grew, frame did not)");
    }

    // 样式功能的另一个旋钮：整窗不透明度会给窗口加上 WS_EX_LAYERED。alpha 是合成阶段
    // 施加的，WGC 取的是窗口自身的内容，所以画面不应该被桌面透上来冲淡。
    void captureWithLayeredOpacity(OpenGlWindow& window) {
        using namespace MCDevTool::Style;
        const int pid = static_cast<int>(GetCurrentProcessId());
        require(
            applyStyleToMinecraftWindow(pid, StyleConfig{.windowOpacity = static_cast<uint8_t>(128)}),
            "Applying opacity style failed"
        );
        require(
            (GetWindowLongW(window.hwnd, GWL_EXSTYLE) & WS_EX_LAYERED) != 0,
            "Opacity style should have made the window layered"
        );
        for (int i = 0; i < 3; ++i) {
            window.render();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }

        UINT width = 0, height = 0;
        expectedJpegSize(window.hwnd, width, height);
        captureAndVerify(window, width, height, "layered window at 50% opacity");

        // 还原，避免影响后面的用例。
        SetLayeredWindowAttributes(window.hwnd, 0, 255, LWA_ALPHA);
        SetWindowLongW(window.hwnd, GWL_EXSTYLE, GetWindowLongW(window.hwnd, GWL_EXSTYLE) & ~WS_EX_LAYERED);
    }

    int captureWithoutCallerCom(int pid) {
        // 必须在独立进程执行，避免父测试进程的 MTA 掩盖截图线程注销后的崩溃。
        APTTYPE          apartmentType{};
        APTTYPEQUALIFIER qualifier{};
        require(
            CoGetApartmentType(&apartmentType, &qualifier) == CO_E_NOTINITIALIZED,
            "Capture regression requires an uninitialized caller"
        );
        for (int attempt = 0; attempt < 3; ++attempt) {
            auto result = MCDevTool::Style::captureMinecraftWindowJpeg(pid);
            require(
                result && result->size() > 2 && (*result)[0] == 0xff && (*result)[1] == 0xd8,
                "Capture without caller COM initialization failed"
            );
            // 旧实现已返回 JPEG，但稍后的 WGC 后台任务会执行已卸载 DLL 中的代码。
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        require(!MCDevTool::Style::captureMinecraftWindowJpeg(-1), "Unknown PID must fail");
        std::this_thread::sleep_for(std::chrono::seconds(2));
        return 0;
    }

    void verifyCaptureWithoutCallerCom(OpenGlWindow& window) {
        wchar_t executable[32768]{};
        require(GetModuleFileNameW(nullptr, executable, 32768) != 0, "GetModuleFileName failed");
        std::wstring command =
            L"\"" + std::wstring(executable) + L"\" --capture-without-com " + std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        require(
            CreateProcessW(
                executable,
                command.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &startup,
                &process
            ),
            "Create capture regression process failed"
        );
        winrt::handle processHandle{process.hProcess};
        winrt::handle threadHandle{process.hThread};
        try {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
            while (true) {
                const auto status = WaitForSingleObject(processHandle.get(), 16);
                if (status == WAIT_OBJECT_0) {
                    break;
                }
                require(status == WAIT_TIMEOUT, "Wait for capture regression failed");
                require(std::chrono::steady_clock::now() < deadline, "Capture regression timed out");
                window.render();
            }
            DWORD exitCode = 0;
            require(GetExitCodeProcess(processHandle.get(), &exitCode), "Get capture regression exit code failed");
            if (exitCode != 0) {
                std::cerr << "Capture regression process exited with 0x" << std::hex << exitCode << std::dec << '\n';
            }
            require(exitCode == 0, "Capture process crashed after returning a screenshot");
        } catch (...) {
            if (WaitForSingleObject(processHandle.get(), 0) == WAIT_TIMEOUT) {
                TerminateProcess(processHandle.get(), 1);
                WaitForSingleObject(processHandle.get(), 5000);
            }
            throw;
        }
        std::cout << "PASS: repeated capture without caller COM / delayed cleanup / process exit\n";
    }
} // namespace

int main(int argc, char* argv[]) {
    if (argc == 3 && std::string_view(argv[1]) == "--capture-without-com") {
        try {
            return captureWithoutCallerCom(std::stoi(argv[2]));
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    winrt::init_apartment();
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int result = 0;
    try {
        if (!winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported()) {
            std::cout << "SKIP: Windows Graphics Capture unavailable\n";
            winrt::uninit_apartment();
            return 77;
        }
        OpenGlWindow window;
        window.create();
        for (int i = 0; i < 3; ++i) {
            window.render();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        verifyCaptureWithoutCallerCom(window);
        captureAndVerify(window, 640, 480, "OpenGL SwapBuffers / 480p / client crop");

        Occluder cover;
        cover.hwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            L"STATIC",
            L"WGC test occluder",
            WS_POPUP | WS_VISIBLE | SS_WHITERECT,
            50,
            50,
            1000,
            850,
            nullptr,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr
        );
        require(cover.hwnd != nullptr, "Create occluder failed");
        UpdateWindow(cover.hwnd);
        captureAndVerify(window, 640, 480, "fully occluded OpenGL window");
        captureAndVerify(window, 640, 480, "repeated capture / resource cleanup");
        captureAndVerify(window, 640, 360, "resize during capture / no upscaling", true);
        captureDuringContinuousResize(window);
        captureRegionsAndLimits(window);
        captureAfterHidingTitleBar(window);
        captureWithLayeredOpacity(window);

        SetWindowLongW(window.hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        window.resize(400, 300);
        captureAndVerify(window, 400, 300, "borderless window");

        ShowWindow(window.hwnd, SW_MINIMIZE);
        const auto minimized = MCDevTool::Style::captureMinecraftWindowJpeg(static_cast<int>(GetCurrentProcessId()));
        require(
            !minimized.has_value() && minimized.error() == MCDevTool::Style::CaptureError::WindowMinimized,
            "Minimized window must report WindowMinimized"
        );
        const auto missing = MCDevTool::Style::captureMinecraftWindowJpeg(-1);
        require(
            !missing.has_value() && missing.error() == MCDevTool::Style::CaptureError::WindowNotFound,
            "Unknown PID must report WindowNotFound"
        );
        std::cout << "PASS: minimized / unknown PID report distinct reasons\n";
    } catch (const winrt::hresult_error& error) {
        std::cerr << winrt::to_string(error.message()) << '\n';
        result = 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    winrt::uninit_apartment();
    return result;
}
