#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "window_capture.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <thread>
#include <d3d11.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

namespace MCDevTool::Style::Detail {
    namespace {
        using namespace winrt::Windows::Graphics;
        using namespace winrt::Windows::Graphics::Capture;
        using namespace winrt::Windows::Graphics::DirectX;
        using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

        // 成功、超时和异常路径都显式关闭捕获资源。
        template <typename T>
        struct CaptureResource {
            T value{nullptr};
            ~CaptureResource() {
                if (value) {
                    try {
                        value.Close();
                    } catch (...) {}
                }
            }
        };

        struct Apartment {
            Apartment() {
                winrt::init_apartment(winrt::apartment_type::multi_threaded);
            }
            ~Apartment() {
                winrt::uninit_apartment();
            }
        };

        void keepCaptureModuleLoaded(IUnknown* factory) {
            // WGC 的 Close 返回后仍可能有系统后台任务。最后一个 MTA 注销时卸载
            // 实现 DLL，会使这些任务执行已卸载的代码，导致整个宿主进程访问冲突。
            // 根据工厂虚表定位实际实现模块，固定到进程结束，不依赖系统 DLL 文件名。
            [[maybe_unused]] static const HMODULE captureModule = [factory] {
                const auto vtable = *reinterpret_cast<const void* const* const*>(factory);
                HMODULE    module = nullptr;
                winrt::check_bool(GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                    reinterpret_cast<LPCWSTR>(vtable),
                    &module
                ));
                return module;
            }();
        }

        struct DpiContext {
            DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            ~DpiContext() {
                if (previous) {
                    SetThreadDpiAwarenessContext(previous);
                }
            }
        };

        // WGC 不包含不可见的缩放边框，GetWindowRect 则包含。
        // 使用 DWM 的物理边界，确保截图与客户区点击坐标一致。
        std::optional<RECT> clientCrop(HWND hwnd, SizeInt32 size) {
            RECT  bounds{}, client{};
            POINT origin{};
            if (!IsWindow(hwnd) || IsIconic(hwnd)
                || FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))
                || !GetClientRect(hwnd, &client) || !ClientToScreen(hwnd, &origin)) {
                return std::nullopt;
            }
            if (bounds.right - bounds.left != size.Width || bounds.bottom - bounds.top != size.Height) {
                return std::nullopt; // 窗口几何信息已变化，等待尺寸匹配的新帧。
            }
            RECT crop{
                origin.x - bounds.left,
                origin.y - bounds.top,
                origin.x - bounds.left + client.right,
                origin.y - bounds.top + client.bottom
            };
            if (crop.left < 0 || crop.top < 0 || crop.right > size.Width || crop.bottom > size.Height
                || crop.right <= crop.left || crop.bottom <= crop.top) {
                return std::nullopt;
            }
            return crop;
        }

        std::vector<uint8_t>
        encodeJpeg(IWICImagingFactory* factory, IWICBitmapSource* bitmap, UINT width, UINT height) {
            const UINT targetHeight = std::min(height, 480u);
            const UINT targetWidth =
                std::max(1u, static_cast<UINT>((static_cast<uint64_t>(width) * targetHeight + height / 2) / height));
            winrt::com_ptr<IWICBitmapScaler> scaler;
            winrt::check_hresult(factory->CreateBitmapScaler(scaler.put()));
            winrt::check_hresult(scaler->Initialize(bitmap, targetWidth, targetHeight, WICBitmapInterpolationModeFant));

            winrt::com_ptr<IWICFormatConverter> converter;
            winrt::check_hresult(factory->CreateFormatConverter(converter.put()));
            winrt::check_hresult(converter->Initialize(
                scaler.get(),
                GUID_WICPixelFormat24bppBGR,
                WICBitmapDitherTypeNone,
                nullptr,
                0,
                WICBitmapPaletteTypeCustom
            ));

            winrt::com_ptr<IStream> stream;
            winrt::check_hresult(CreateStreamOnHGlobal(nullptr, TRUE, stream.put()));
            winrt::com_ptr<IWICBitmapEncoder> encoder;
            winrt::check_hresult(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, encoder.put()));
            winrt::check_hresult(encoder->Initialize(stream.get(), WICBitmapEncoderNoCache));
            winrt::com_ptr<IWICBitmapFrameEncode> frame;
            winrt::com_ptr<IPropertyBag2>         options;
            winrt::check_hresult(encoder->CreateNewFrame(frame.put(), options.put()));
            PROPBAG2 property{};
            property.pstrName = const_cast<wchar_t*>(L"ImageQuality");
            VARIANT quality{};
            quality.vt     = VT_R4;
            quality.fltVal = 0.75f;
            winrt::check_hresult(options->Write(1, &property, &quality));
            winrt::check_hresult(frame->Initialize(options.get()));
            winrt::check_hresult(frame->SetSize(targetWidth, targetHeight));
            WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
            winrt::check_hresult(frame->SetPixelFormat(&format));
            winrt::check_bool(IsEqualGUID(format, GUID_WICPixelFormat24bppBGR));
            winrt::check_hresult(frame->WriteSource(converter.get(), nullptr));
            winrt::check_hresult(frame->Commit());
            winrt::check_hresult(encoder->Commit());

            STATSTG stat{};
            winrt::check_hresult(stream->Stat(&stat, STATFLAG_NONAME));
            winrt::check_bool(stat.cbSize.QuadPart > 0 && stat.cbSize.QuadPart <= std::numeric_limits<ULONG>::max());
            std::vector<uint8_t> result(static_cast<size_t>(stat.cbSize.QuadPart));
            winrt::check_hresult(stream->Seek({}, STREAM_SEEK_SET, nullptr));
            ULONG bytesRead = 0;
            winrt::check_hresult(stream->Read(result.data(), static_cast<ULONG>(result.size()), &bytesRead));
            winrt::check_bool(bytesRead == result.size());
            return result;
        }

        winrt::com_ptr<IWICBitmap> copyClientBitmap(
            ID3D11Device*                 device,
            ID3D11DeviceContext*          context,
            IWICImagingFactory*           factory,
            const Direct3D11CaptureFrame& frame,
            SizeInt32                     size,
            const RECT&                   crop
        ) {
            auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            winrt::com_ptr<ID3D11Texture2D> texture;
            winrt::check_hresult(access->GetInterface(__uuidof(ID3D11Texture2D), texture.put_void()));
            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            if (desc.Width < static_cast<UINT>(size.Width) || desc.Height < static_cast<UINT>(size.Height)) {
                return nullptr;
            }
            desc.Width          = static_cast<UINT>(crop.right - crop.left);
            desc.Height         = static_cast<UINT>(crop.bottom - crop.top);
            desc.MipLevels      = 1;
            desc.ArraySize      = 1;
            desc.SampleDesc     = {1, 0};
            desc.Usage          = D3D11_USAGE_STAGING;
            desc.BindFlags      = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags      = 0;
            winrt::com_ptr<ID3D11Texture2D> staging;
            winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, staging.put()));
            D3D11_BOX box{
                static_cast<UINT>(crop.left),
                static_cast<UINT>(crop.top),
                0,
                static_cast<UINT>(crop.right),
                static_cast<UINT>(crop.bottom),
                1
            };
            context->CopySubresourceRegion(staging.get(), 0, 0, 0, 0, texture.get(), 0, &box);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            winrt::check_hresult(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
            struct Unmap {
                ID3D11DeviceContext* context;
                ID3D11Texture2D*     texture;
                ~Unmap() {
                    context->Unmap(texture, 0);
                }
            } unmap{context, staging.get()};

            // WIC 按 GPU 行跨度复制像素，保留可能存在的行尾填充。
            const uint64_t byteCount = static_cast<uint64_t>(mapped.RowPitch) * desc.Height;
            winrt::check_bool(byteCount <= std::numeric_limits<UINT>::max());
            winrt::com_ptr<IWICBitmap> bitmap;
            winrt::check_hresult(factory->CreateBitmapFromMemory(
                desc.Width,
                desc.Height,
                GUID_WICPixelFormat32bppBGRA,
                mapped.RowPitch,
                static_cast<UINT>(byteCount),
                static_cast<BYTE*>(mapped.pData),
                bitmap.put()
            ));
            return bitmap;
        }

        std::optional<std::vector<uint8_t>> captureOnWorker(HWND hwnd) {
            Apartment  apartment;
            DpiContext dpi;
            if (!IsWindow(hwnd) || IsIconic(hwnd) || !GraphicsCaptureSession::IsSupported()) {
                return std::nullopt;
            }

            auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
            keepCaptureModuleLoaded(interop.get());
            GraphicsCaptureItem item{nullptr};
            winrt::check_hresult(
                interop->CreateForWindow(hwnd, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item))
            );
            auto poolSize = item.Size();
            if (poolSize.Width <= 0 || poolSize.Height <= 0) {
                return std::nullopt;
            }

            winrt::com_ptr<ID3D11Device>        device;
            winrt::com_ptr<ID3D11DeviceContext> context;
            auto                                hr = D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                nullptr,
                0,
                D3D11_SDK_VERSION,
                device.put(),
                nullptr,
                context.put()
            );
            if (FAILED(hr)) {
                device  = nullptr;
                context = nullptr;
                hr      = D3D11CreateDevice(
                    nullptr,
                    D3D_DRIVER_TYPE_WARP,
                    nullptr,
                    D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                    nullptr,
                    0,
                    D3D11_SDK_VERSION,
                    device.put(),
                    nullptr,
                    context.put()
                );
            }
            winrt::check_hresult(hr);
            winrt::com_ptr<IInspectable> inspectable;
            winrt::check_hresult(
                CreateDirect3D11DeviceFromDXGIDevice(device.as<IDXGIDevice>().get(), inspectable.put())
            );
            auto captureDevice = inspectable.as<IDirect3DDevice>();

            // MCP 可从任意线程调用，无需调用者提供 DispatcherQueue 或消息循环。
            CaptureResource<Direct3D11CaptureFramePool> pool{Direct3D11CaptureFramePool::CreateFreeThreaded(
                captureDevice,
                DirectXPixelFormat::B8G8R8A8UIntNormalized,
                2,
                poolSize
            )};
            CaptureResource<GraphicsCaptureSession>     session{pool.value.CreateCaptureSession(item)};
            if (auto cursorSettings = session.value.try_as<IGraphicsCaptureSession2>()) {
                cursorSettings.IsCursorCaptureEnabled(false);
            }
            session.value.StartCapture();

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < deadline) {
                if (!IsWindow(hwnd) || IsIconic(hwnd)) {
                    return std::nullopt;
                }
                CaptureResource<Direct3D11CaptureFrame> frame{pool.value.TryGetNextFrame()};
                if (!frame.value) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                const auto size = frame.value.ContentSize();
                if (size.Width <= 0 || size.Height <= 0) {
                    continue;
                }
                if (size.Width != poolSize.Width || size.Height != poolSize.Height) {
                    frame.value.Close();
                    frame.value = nullptr;
                    poolSize    = size;
                    pool.value.Recreate(captureDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, poolSize);
                    continue;
                }
                auto crop = clientCrop(hwnd, size);
                if (!crop) {
                    continue;
                }

                auto factory = winrt::create_instance<IWICImagingFactory>(CLSID_WICImagingFactory);
                auto bitmap  = copyClientBitmap(device.get(), context.get(), factory.get(), frame.value, size, *crop);
                if (!bitmap) {
                    continue;
                }
                return encodeJpeg(
                    factory.get(),
                    bitmap.get(),
                    static_cast<UINT>(crop->right - crop->left),
                    static_cast<UINT>(crop->bottom - crop->top)
                );
            }
            return std::nullopt;
        }
    } // namespace

    std::optional<std::vector<uint8_t>> captureWindow480p(HWND hwnd) {
        // 隔离 WinRT 的 MTA 要求，兼容已初始化 STA 的调用线程。
        std::optional<std::vector<uint8_t>> result;
        std::thread                         worker([&] {
            try {
                result = captureOnWorker(hwnd);
            } catch (const winrt::hresult_error&) {
                // 捕获不受支持、访问被拒绝、窗口关闭或设备丢失时沿用失败返回值。
                result = std::nullopt;
            } catch (const std::exception&) {
                result = std::nullopt;
            }
        });
        worker.join();
        return result;
    }
} // namespace MCDevTool::Style::Detail
#endif
