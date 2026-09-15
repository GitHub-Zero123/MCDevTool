# Minecraft 窗口截图

`Style::captureMinecraftWindow480p` 使用 Windows.Graphics.Capture（WGC）按 HWND 捕获窗口，通过 D3D11 读回客户区像素，再使用 WIC 缩放、编码 JPEG。

- 仅使用 WGC，不调用 `PrintWindow`、`BitBlt`，不要求游戏响应 GDI 绘制消息。
- 支持窗口被其他窗口遮挡；不会主动置顶、激活或恢复游戏窗口。
- 按物理像素裁剪客户区，排除标题栏和边框，与点击接口的客户区坐标保持一致。
- 保持宽高比，高度最多 480 像素，小窗口不放大，JPEG 质量为 75%。
- 内部使用独立 MTA 线程及 `CreateFreeThreaded` 帧池，调用线程不需要 WinRT 初始化或消息循环。
- 首次捕获时将 WGC 工厂所在的实现模块固定到进程退出，避免最后一个 MTA 注销后，后台任务执行已卸载 DLL 中的代码。帧、会话、设备和线程仍按每次请求正常释放；模块固定使用 Win32 的 `GET_MODULE_HANDLE_EX_FLAG_PIN`，不依赖延时等待。
- 窗口尺寸变化时重建帧池；取帧最多等待 3 秒。窗口不存在、最小化、捕获不可用或失败时返回 `std::nullopt`。
- 当前输出为 SDR JPEG，未实现 HDR 色调映射。

## 环境

运行需要 Windows 10 1903 或更新版本，以及系统允许使用 WGC。系统可能在捕获时显示截图边框。

编译使用带 C++/WinRT、WGC 头文件的 Windows SDK；当前已在 SDK 10.0.26100.0 上验证。CMake 和 xmake 均已声明 D3D11、WinRT、WIC 链接依赖。

## 验证

开启 `MC_DEV_TOOL_BUILD_TEST` 后，构建并运行 `window_capture_test`。该测试需要交互式桌面，会短暂创建 OpenGL 窗口和遮挡窗口；不注册为默认 CTest 用例。

测试使用 `SwapBuffers` 提交红蓝画面，并解码 JPEG 校验尺寸和像素，覆盖完全遮挡、重复截图、尺寸变化、无标题栏、最小化和无效 PID，也检查捕获没有发送 `WM_PRINT` / `WM_PRINTCLIENT`。

另外在独立子进程中覆盖调用方未初始化 COM 的情况：截图返回后保持进程运行、重复捕获并检查正常退出。必须隔离进程，否则父测试中的 MTA 会掩盖 DLL 提前卸载的问题。旧实现在该测试中以 `0xC0000005` 退出。

实际游戏验收：启动 Minecraft，运行 `captureTest.exe [输出.jpg]`，分别在可见和被其他窗口完全遮挡的状态下检查输出。该工具只保存截图，不点击游戏窗口。

## API 依据

- [CreateForWindow：按窗口句柄创建捕获对象及系统要求](https://learn.microsoft.com/en-us/windows/win32/api/windows.graphics.capture.interop/nf-windows-graphics-capture-interop-igraphicscaptureiteminterop-createforwindow)
- [CreateFreeThreaded：无需 DispatcherQueue 的帧池](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframepool.createfreethreaded)
- [Screen capture：帧尺寸、资源释放与重建](https://learn.microsoft.com/en-us/windows/apps/develop/media-authoring-processing/screen-capture)
- [GetModuleHandleExW：将实现模块固定到进程退出](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getmodulehandleexw)
- [WGC 注销 COM 时提前卸载模块的同类问题](https://github.com/robmikh/Win32CaptureSample/issues/99)
