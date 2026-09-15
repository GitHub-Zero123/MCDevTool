#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <gl/GL.h>
#include <wincodec.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>

#include <array>
#include <chrono>
#include <future>
#include <fstream>
#include <iostream>
#include <stdexcept>
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
            auto result = MCDevTool::Style::captureMinecraftWindow480p(static_cast<int>(GetCurrentProcessId()));
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
} // namespace

int main() {
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

        SetWindowLongW(window.hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        window.resize(400, 300);
        captureAndVerify(window, 400, 300, "borderless window");

        ShowWindow(window.hwnd, SW_MINIMIZE);
        require(
            !MCDevTool::Style::captureMinecraftWindow480p(static_cast<int>(GetCurrentProcessId())),
            "Minimized window must fail"
        );
        require(!MCDevTool::Style::captureMinecraftWindow480p(-1), "Unknown PID must fail");
        std::cout << "PASS: minimized / unknown PID\n";
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
