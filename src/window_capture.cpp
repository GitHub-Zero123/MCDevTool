#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "window_capture.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <expected>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
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

        // DPI 取整、圆角阴影等会让窗口几何与帧尺寸差出 1 像素。严格相等会让这些环境
        // 永远等不到可用的帧，因此允许一个像素级别的偏差。
        constexpr LONG kGeometryTolerance = 1;

        // 取帧总时限。前 kStrictGeometryWindow 内只接受几何完全匹配的帧；之后退化为
        // 按比例换算裁剪，宁可有一点偏差也不要交白卷。
        //
        // 实测首帧在 20ms 内到达，干净的一帧真要来也就是几帧的事。严格窗口开得太长没有
        // 收益，反而会让"系统性对不上"的环境每张截图都先白等满这段时间才走兜底。
        constexpr auto kCaptureTimeout       = std::chrono::seconds(3);
        constexpr auto kStrictGeometryWindow = std::chrono::milliseconds(500);

        // 调用方等待捕获线程的上限。取帧循环自带时限，但 WinRT 激活和 D3D 设备创建不
        // 在其内，驱动挂死时不能让 MCP 请求线程无限期阻塞。
        constexpr auto kWorkerTimeout = std::chrono::seconds(10);

        using CaptureResult = std::expected<std::vector<uint8_t>, CaptureError>;

        bool validRegion(const CaptureRegion& region) {
            return region.left >= 0.0 && region.top >= 0.0 && region.right <= 1.0 && region.bottom <= 1.0
                && region.left < region.right && region.top < region.bottom;
        }

        // 存储与进程同寿、析构被刻意跳过的单例包装。Chromium 的 base::NoDestructor 和
        // Abseil 的 absl::NoDestructor 是同一个东西，为的就是这个场景。
        //
        // 常驻捕获线程会一直持有里面的互斥量和条件变量，进程退出前不存在"没人在用"的
        // 时刻。若让它随静态析构销毁，工作线程很可能正阻塞在条件变量上，而销毁有等待者
        // 的条件变量是未定义行为；在析构里 join 也不行——线程退出就离开了 MTA，等于在
        // 退出路径上把 WGC 的 DLL 卸载问题请回来。
        //
        // 对象放在自身的内联存储里而不是堆上：泄漏检测器不会报，且包装类型可平凡析构、
        // 不会注册 atexit，因此静态析构期之后取用仍然安全。
        template <typename T>
        class NoDestructor {
        public:
            template <typename... Args>
            explicit NoDestructor(Args&&... args) {
                ::new (static_cast<void*>(mStorage)) T(std::forward<Args>(args)...);
            }
            NoDestructor(const NoDestructor&)            = delete;
            NoDestructor& operator=(const NoDestructor&) = delete;

            T* operator->() { return get(); }
            T& operator*() { return *get(); }
            T* get() { return reinterpret_cast<T*>(mStorage); }

        private:
            alignas(T) unsigned char mStorage[sizeof(T)];
        };

        void ensure(bool condition, winrt::hresult result) {
            if (!condition) {
                winrt::throw_hresult(result);
            }
        }

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

        void keepCaptureModuleLoaded(IUnknown* factory) {
            // 主要保证来自常驻 MTA 线程：MTA 永不注销，WGC 的实现 DLL 就不会在后台任务
            // 仍在运行时被卸载。这里额外把工厂所在模块固定住，成本只有一次
            // GetModuleHandleExW，作为该类崩溃的兜底。
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
        //
        // allowApproximate 为真时进入兜底模式：几何对不上就按比例换算，完全拿不到几何
        // 信息就返回整帧。严格模式下这些情况一律返回空，等待下一帧。
        std::optional<RECT> clientCrop(HWND hwnd, SizeInt32 size, bool allowApproximate) {
            if (!IsWindow(hwnd) || IsIconic(hwnd)) {
                return std::nullopt;
            }
            const RECT wholeFrame{0, 0, size.Width, size.Height};
            const auto fallback = [&]() -> std::optional<RECT> {
                return allowApproximate ? std::optional{wholeFrame} : std::nullopt;
            };

            RECT  bounds{}, client{};
            POINT origin{};
            if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))
                || !GetClientRect(hwnd, &client) || !ClientToScreen(hwnd, &origin)) {
                return fallback();
            }
            const LONG boundsWidth  = bounds.right - bounds.left;
            const LONG boundsHeight = bounds.bottom - bounds.top;
            if (boundsWidth <= 0 || boundsHeight <= 0 || client.right <= 0 || client.bottom <= 0) {
                return fallback();
            }
            if (std::abs(boundsWidth - wholeFrame.right) > kGeometryTolerance
                || std::abs(boundsHeight - wholeFrame.bottom) > kGeometryTolerance) {
                if (!allowApproximate) {
                    return std::nullopt; // 窗口几何信息已变化，等待尺寸匹配的新帧。
                }
            }

            // 按比例把客户区映射到帧上。尺寸一致时缩放因子为 1，结果与严格路径逐像素相同。
            const auto map = [](LONG value, LONG from, LONG to) {
                return static_cast<LONG>((static_cast<int64_t>(value) * to + from / 2) / from);
            };
            const LONG left = origin.x - bounds.left;
            const LONG top  = origin.y - bounds.top;
            const RECT crop{
                std::clamp(map(left, boundsWidth, wholeFrame.right), 0L, wholeFrame.right),
                std::clamp(map(top, boundsHeight, wholeFrame.bottom), 0L, wholeFrame.bottom),
                std::clamp(map(left + client.right, boundsWidth, wholeFrame.right), 0L, wholeFrame.right),
                std::clamp(map(top + client.bottom, boundsHeight, wholeFrame.bottom), 0L, wholeFrame.bottom)
            };
            if (crop.right <= crop.left || crop.bottom <= crop.top) {
                return fallback();
            }
            return crop;
        }

        // 把归一化区域映射进客户区裁剪框。默认整块客户区时原样返回。
        std::optional<RECT> applyRegion(const RECT& client, const CaptureRegion& region) {
            if (!validRegion(region)) {
                return std::nullopt;
            }
            const LONG width  = client.right - client.left;
            const LONG height = client.bottom - client.top;
            const auto mapX   = [&](double value) {
                return client.left + static_cast<LONG>(std::llround(value * static_cast<double>(width)));
            };
            const auto mapY = [&](double value) {
                return client.top + static_cast<LONG>(std::llround(value * static_cast<double>(height)));
            };
            RECT crop{mapX(region.left), mapY(region.top), mapX(region.right), mapY(region.bottom)};
            // 区域很小时映射后可能塌成零像素，至少保留一个像素。
            if (crop.right <= crop.left) {
                crop.right = std::min(crop.left + 1, client.right);
            }
            if (crop.bottom <= crop.top) {
                crop.bottom = std::min(crop.top + 1, client.bottom);
            }
            if (crop.right <= crop.left || crop.bottom <= crop.top) {
                return std::nullopt;
            }
            return crop;
        }

        std::vector<uint8_t> encodeJpeg(
            IWICImagingFactory* factory,
            IWICBitmapSource*   bitmap,
            UINT                width,
            UINT                height,
            unsigned            maxHeight
        ) {
            const UINT targetHeight = std::min(height, std::max(1u, maxHeight));
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
            ensure(IsEqualGUID(format, GUID_WICPixelFormat24bppBGR), WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT);
            winrt::check_hresult(frame->WriteSource(converter.get(), nullptr));
            winrt::check_hresult(frame->Commit());
            winrt::check_hresult(encoder->Commit());

            STATSTG stat{};
            winrt::check_hresult(stream->Stat(&stat, STATFLAG_NONAME));
            ensure(stat.cbSize.QuadPart > 0 && stat.cbSize.QuadPart <= std::numeric_limits<ULONG>::max(), E_UNEXPECTED);
            std::vector<uint8_t> result(static_cast<size_t>(stat.cbSize.QuadPart));
            winrt::check_hresult(stream->Seek({}, STREAM_SEEK_SET, nullptr));
            ULONG bytesRead = 0;
            winrt::check_hresult(stream->Read(result.data(), static_cast<ULONG>(result.size()), &bytesRead));
            ensure(bytesRead == result.size(), E_UNEXPECTED);
            return result;
        }

        // CreateBitmapFromMemory 没有文档化缓冲区所有权，不能假设它一定复制像素。映射
        // 因此保持到 WIC 编码结束，由这个对象统一解除。
        struct MappedBitmap {
            winrt::com_ptr<ID3D11DeviceContext> context;
            winrt::com_ptr<ID3D11Texture2D>     staging;
            winrt::com_ptr<IWICBitmap>          bitmap;

            MappedBitmap()                               = default;
            MappedBitmap(const MappedBitmap&)            = delete;
            MappedBitmap& operator=(const MappedBitmap&) = delete;
            ~MappedBitmap() {
                if (context && staging) {
                    context->Unmap(staging.get(), 0);
                }
            }
        };

        std::unique_ptr<MappedBitmap> copyClientBitmap(
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

            auto result = std::make_unique<MappedBitmap>();
            winrt::check_hresult(device->CreateTexture2D(&desc, nullptr, result->staging.put()));
            D3D11_BOX box{
                static_cast<UINT>(crop.left),
                static_cast<UINT>(crop.top),
                0,
                static_cast<UINT>(crop.right),
                static_cast<UINT>(crop.bottom),
                1
            };
            context->CopySubresourceRegion(result->staging.get(), 0, 0, 0, 0, texture.get(), 0, &box);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            winrt::check_hresult(context->Map(result->staging.get(), 0, D3D11_MAP_READ, 0, &mapped));
            result->context.copy_from(context); // 映射已生效，之后任何路径都会解除。

            // WIC 按 GPU 行跨度读取像素，保留可能存在的行尾填充。
            const uint64_t byteCount = static_cast<uint64_t>(mapped.RowPitch) * desc.Height;
            ensure(byteCount <= std::numeric_limits<UINT>::max(), E_INVALIDARG);
            winrt::check_hresult(factory->CreateBitmapFromMemory(
                desc.Width,
                desc.Height,
                GUID_WICPixelFormat32bppBGRA,
                mapped.RowPitch,
                static_cast<UINT>(byteCount),
                static_cast<BYTE*>(mapped.pData),
                result->bitmap.put()
            ));
            return result;
        }

        CaptureResult captureOnWorker(HWND hwnd, CaptureOptions options) {
            DpiContext dpi;
            if (!IsWindow(hwnd)) {
                return std::unexpected{CaptureError::WindowNotFound};
            }
            if (IsIconic(hwnd)) {
                return std::unexpected{CaptureError::WindowMinimized};
            }
            if (!GraphicsCaptureSession::IsSupported()) {
                return std::unexpected{CaptureError::CaptureUnavailable};
            }

            auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
            keepCaptureModuleLoaded(interop.get());
            GraphicsCaptureItem item{nullptr};
            winrt::check_hresult(
                interop->CreateForWindow(hwnd, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item))
            );
            auto poolSize = item.Size();
            if (poolSize.Width <= 0 || poolSize.Height <= 0) {
                return std::unexpected{CaptureError::Failed};
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

            // 捕获线程是常驻 MTA，调用方无需提供 DispatcherQueue 或消息循环。
            CaptureResource<Direct3D11CaptureFramePool> pool{Direct3D11CaptureFramePool::CreateFreeThreaded(
                captureDevice,
                DirectXPixelFormat::B8G8R8A8UIntNormalized,
                2,
                poolSize
            )};
            CaptureResource<GraphicsCaptureSession>     session{pool.value.CreateCaptureSession(item)};

            // 保留系统指针。游戏在背包、菜单和 F11 触屏模拟下用的就是系统光标，去掉它会让
            // 截图看不出指针位置，也和 mc_input 真正移动过的光标对不上。WGC 默认即包含
            // 光标，因此不再按 IGraphicsCaptureSession2 做版本分支。
            session.value.StartCapture();

            auto       factory        = winrt::create_instance<IWICImagingFactory>(CLSID_WICImagingFactory);
            const auto start          = std::chrono::steady_clock::now();
            const auto strictDeadline = start + kStrictGeometryWindow;
            const auto deadline       = start + kCaptureTimeout;
            while (std::chrono::steady_clock::now() < deadline) {
                if (!IsWindow(hwnd)) {
                    return std::unexpected{CaptureError::WindowNotFound};
                }
                if (IsIconic(hwnd)) {
                    return std::unexpected{CaptureError::WindowMinimized};
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
                if (size.Width > poolSize.Width || size.Height > poolSize.Height) {
                    // 内容比帧池大，这一帧的纹理装不下完整画面：重建后重试。
                    frame.value.Close();
                    frame.value = nullptr;
                    poolSize    = size;
                    pool.value.Recreate(captureDevice, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, poolSize);
                    continue;
                }
                // 内容比帧池小（窗口刚缩小）时纹理依然够大，按 ContentSize 裁剪即可，不必丢帧。

                const bool allowApproximate = std::chrono::steady_clock::now() >= strictDeadline;
                const auto crop             = clientCrop(hwnd, size, allowApproximate);
                if (!crop) {
                    continue;
                }
                const auto region = applyRegion(*crop, options.region);
                if (!region) {
                    return std::unexpected{CaptureError::InvalidRegion};
                }
                const auto mapped =
                    copyClientBitmap(device.get(), context.get(), factory.get(), frame.value, size, *region);
                if (!mapped) {
                    continue;
                }
                return encodeJpeg(
                    factory.get(),
                    mapped->bitmap.get(),
                    static_cast<UINT>(region->right - region->left),
                    static_cast<UINT>(region->bottom - region->top),
                    options.maxHeight
                );
            }
            return std::unexpected{CaptureError::Timeout};
        }

        // WGC 的实现 DLL 会在最后一个 MTA 线程注销时卸载，而此时系统后台任务可能仍在运
        // 行，旧实现因此在宿主进程里以 0xC0000005 退出。这里改为进程内保留一条常驻 MTA
        // 线程：只初始化一次、永不注销，所有捕获请求排队到它上面执行。
        //
        // 线程在第一次真正截图时才创建，WinRT / WGC 也才在那一刻被激活。只用日志、代码
        // 执行或性能分析的会话不会因为链接了这个文件而多加载任何模块。
        class CaptureWorker {
        public:
            static CaptureResult run(HWND hwnd, CaptureOptions options) {
                // 惰性构造：线程和 WinRT 都到第一次真正截图时才出现。
                static NoDestructor<CaptureWorker> worker;
                return worker->submit(hwnd, options);
            }

        private:
            friend class NoDestructor<CaptureWorker>; // 只允许那一个进程级实例构造。

            struct Request {
                HWND           hwnd{};
                CaptureOptions options;
                CaptureResult  result = std::unexpected{CaptureError::Failed};
                bool           done      = false;
                bool           abandoned = false;
            };

            CaptureWorker() { std::thread([this] { loop(); }).detach(); }

            CaptureResult submit(HWND hwnd, CaptureOptions options) {
                auto request     = std::make_shared<Request>();
                request->hwnd    = hwnd;
                request->options = options;
                {
                    std::lock_guard lock{mMutex};
                    mQueue.push_back(request);
                }
                mPending.notify_one();

                std::unique_lock lock{mMutex};
                // 超时后放弃这次请求，但 Request 由 shared_ptr 持有，工作线程稍后写回结果
                // 依然安全。宁可这一张截图失败，也不让调用线程被驱动挂死拖住。
                if (!mFinished.wait_for(lock, kWorkerTimeout, [&] { return request->done; })) {
                    // 标记为已放弃，工作线程取到时直接跳过。否则一次卡顿攒下的积压会让它
                    // 逐个跑完没人要的截图，后面的请求继续排队超时，把一次故障拖成一串。
                    request->abandoned = true;
                    return std::unexpected{CaptureError::Timeout};
                }
                return std::move(request->result);
            }

            void loop() {
                bool ready = false;
                try {
                    winrt::init_apartment(winrt::apartment_type::multi_threaded);
                    ready = true;
                } catch (...) {
                    // 单元初始化失败时线程依然存活，只是每次请求都直接返回失败，
                    // 不能让调用方永远等不到应答。
                }

                for (;;) {
                    std::shared_ptr<Request> request;
                    {
                        std::unique_lock lock{mMutex};
                        mPending.wait(lock, [&] { return !mQueue.empty(); });
                        request = mQueue.front();
                        mQueue.pop_front();
                        if (request->abandoned) {
                            continue; // 调用方早已超时返回，不必再截这一张。
                        }
                    }

                    CaptureResult result = std::unexpected{
                        ready ? CaptureError::Failed : CaptureError::CaptureUnavailable
                    };
                    if (ready) {
                        try {
                            result = captureOnWorker(request->hwnd, request->options);
                        } catch (const winrt::hresult_error&) {
                            // 捕获不受支持、访问被拒绝、窗口关闭或设备丢失时沿用失败返回值。
                        } catch (const std::exception&) {}
                    }
                    {
                        std::lock_guard lock{mMutex};
                        request->result = std::move(result);
                        request->done   = true;
                    }
                    mFinished.notify_all();
                }
            }

            std::mutex                           mMutex;
            std::condition_variable              mPending;
            std::condition_variable              mFinished;
            std::deque<std::shared_ptr<Request>> mQueue;
        };
    } // namespace

    std::expected<std::vector<uint8_t>, CaptureError> captureWindowJpeg(HWND hwnd, CaptureOptions options) {
        if (!validRegion(options.region)) {
            return std::unexpected{CaptureError::InvalidRegion}; // 不必为一个坏参数开一次捕获。
        }
        // 常驻 MTA 线程隔离了 WinRT 的单元要求，调用线程是 STA 还是未初始化都不受影响。
        try {
            return CaptureWorker::run(hwnd, options);
        } catch (const winrt::hresult_error&) {
            return std::unexpected{CaptureError::Failed};
        } catch (const std::exception&) {
            return std::unexpected{CaptureError::Failed}; // 线程或同步对象创建失败。
        }
    }
} // namespace MCDevTool::Style::Detail
#endif
