# Minecraft 窗口截图

`Style::captureMinecraftWindowJpeg(pid, options)` 使用 Windows.Graphics.Capture（WGC）按 HWND 捕获窗口，通过 D3D11 读回客户区像素，再使用 WIC 缩放、编码 JPEG。返回 `std::expected<std::vector<uint8_t>, CaptureError>`。

- 仅使用 WGC，不调用 `PrintWindow`、`BitBlt`，不要求游戏响应 GDI 绘制消息。
- 支持窗口被其他窗口遮挡；不会主动置顶、激活或恢复游戏窗口。
- 按物理像素裁剪客户区，排除标题栏和边框，与点击接口的客户区坐标保持一致。
- 保持宽高比，高度上限由 `CaptureOptions::maxHeight` 决定（默认 480），小窗口不放大，JPEG 质量为 75%。
- `CaptureOptions::region` 只截客户区里的一块，坐标归一化到 0.0-1.0，与 `mc_input` 的客户区百分比同构：`(0,0)` 左上，`(1,1)` 右下。默认整块客户区。物品数量、tooltip、聊天这类小字在 480p 下通常读不出来；截一块比把整张放大更清楚，数据量还更小。非法区域（越界或左右/上下颠倒）在开捕获之前就返回 `InvalidRegion`。
- 失败返回具体原因而不是单一的空值：`WindowNotFound`、`WindowMinimized`、`CaptureUnavailable`、`Timeout`、`InvalidRegion`、`Failed`。`describeCaptureError` 给出面向调用方的说明，每条都带下一步该怎么做——自动化调用方需要能区分"重试可能有用"、"先恢复窗口"和"这个会话别再截图了"。
- 截图随画面一并包含系统指针。游戏在背包、菜单和 F11 触屏模拟下用的就是系统光标，去掉它会让截图看不出指针位置，也和 `mc_input` 真正移动过的光标对不上。
- 内部使用常驻 MTA 线程及 `CreateFreeThreaded` 帧池，调用线程不需要 WinRT 初始化或消息循环。该线程在第一次截图时才创建，只用日志、代码执行或性能分析的会话不会因此加载 WinRT / WGC。线程与其同步对象用 `NoDestructor`（同 Chromium `base::NoDestructor`）持有：对象放在内联存储里、析构被刻意跳过，因为工作线程在进程退出时仍可能阻塞在条件变量上，而销毁有等待者的条件变量是未定义行为。不走堆，泄漏检测器不会报。
- 捕获线程的 MTA 只初始化一次、永不注销，WGC 的实现 DLL 因此不会在系统后台任务仍在运行时被卸载（旧实现在该场景下以 `0xC0000005` 退出）。帧、会话和设备仍按每次请求正常释放。首次捕获时还会用 `GET_MODULE_HANDLE_EX_FLAG_PIN` 固定工厂所在模块，作为同类崩溃的兜底。
- 不做任何"等游戏画出新一帧"的延迟。取一帧只反映调用时刻的窗口内容，宿主无从知道游戏是否已消费某次输入；需要画面反映操作结果时由调用方显式等待（`mc_input` 的 `wait` 步骤）。
- 内容大于帧池时重建帧池；小于帧池时纹理依然够用，直接按 `ContentSize` 裁剪，不丢帧。
- 取帧最多等待 3 秒。前 500ms 只接受几何与帧尺寸完全匹配（容差 1 像素）的帧（实测首帧 20ms 内到达，干净的一帧真要来就是几帧的事，窗口开长了只会让系统性对不上的环境每张图都白等）；之后转入兜底：按比例把客户区映射到帧上，拿不到 DWM 几何信息就返回整帧。窗口不存在、最小化、捕获不可用或始终没有帧时返回 `std::nullopt`。
- 调用方等待捕获线程的上限为 10 秒。取帧循环自带时限，但 WinRT 激活和 D3D 设备创建不在其内，驱动挂死时不能让调用线程无限期阻塞。请求在这条线程上串行执行。
- 当前输出为 SDR JPEG，未实现 HDR 色调映射。

## 环境

运行需要 Windows 10 1809（`CreateFreeThreaded` 的下限）或更新版本，以及系统允许使用 WGC。

Windows 11 捕获期间必然在目标窗口外画一圈黄色指示边框，且**无法关闭也无法改色**：颜色由 DWM 决定，没有公开 API；关闭需要 `GraphicsCaptureSession.IsBorderRequired`，而它要求先通过 `GraphicsCaptureAccess.RequestAccessAsync(Borderless)` 取得用户同意，该调用又要求在应用包清单里声明 `graphicsCaptureWithoutBorder` 能力。MCDK 是无包标识的普通 Win32 程序，拿不到这个能力。Windows 10 上不显示该边框。

编译使用带 C++/WinRT、WGC 头文件的 Windows SDK；当前已在 SDK 10.0.26100.0 上验证。CMake 和 xmake 均已声明 D3D11、WinRT、WIC 链接依赖。

## 验证

开启 `MC_DEV_TOOL_BUILD_TEST` 后，构建并运行 `window_capture_test`。该测试需要交互式桌面，会短暂创建 OpenGL 窗口和遮挡窗口；不注册为默认 CTest 用例。

测试使用 `SwapBuffers` 提交红蓝画面，并解码 JPEG 校验尺寸和像素，覆盖完全遮挡、重复截图、尺寸变化、连续 resize（帧池重建）、区域截图（上半必须全红、下半必须全蓝，据此验证区域的偏移和方向）、`maxHeight`、非法区域、无标题栏，以及最小化和无效 PID 各自返回的错误码，也检查捕获没有发送 `WM_PRINT` / `WM_PRINTCLIENT`。

与窗口样式功能的交互单独覆盖，直接调用 `applyStyleToMinecraftWindow` 而不是在测试里重写隐藏逻辑：

- **运行时摘掉标题栏**。`hideTitleBar` 走的是 `SWP_NOSIZE | SWP_FRAMECHANGED`，窗口外框尺寸不变、只有客户区变大，而裁剪的几何严格比对比的正是外框尺寸，发现不了这种变化。用例断言截图尺寸跟上了新的客户区，且上半仍是红、下半仍是蓝——裁到旧帧的话顶部取到的会是标题栏。需要说明的是它验证的是真实用法（游戏持续出帧）：捕获本身约 100ms 的会话建立时间足以让应用重画完新的客户区。应用停止重绘时截到的就是旧内容，那是窗口当时的真实样子，不是裁剪的错。
- **整窗不透明度**。`windowOpacity` 会给窗口加 `WS_EX_LAYERED`。用例确认 alpha 是合成阶段施加的，WGC 取到的是窗口自身的内容，画面不会被桌面透上来冲淡（红色分量仍在阈值以上）。

另外在独立子进程中覆盖调用方未初始化 COM 的情况：截图返回后保持进程运行、重复捕获并检查正常退出。必须隔离进程，否则父测试中的 MTA 会掩盖 DLL 提前卸载的问题。旧实现在该测试中以 `0xC0000005` 退出。

实际游戏验收：启动 Minecraft，运行 `captureTest.exe [输出.jpg]`，分别在可见和被其他窗口完全遮挡的状态下检查输出。该工具只保存截图，不点击游戏窗口。

## API 依据

- [CreateForWindow：按窗口句柄创建捕获对象及系统要求](https://learn.microsoft.com/en-us/windows/win32/api/windows.graphics.capture.interop/nf-windows-graphics-capture-interop-igraphicscaptureiteminterop-createforwindow)
- [CreateFreeThreaded：无需 DispatcherQueue 的帧池](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.capture.direct3d11captureframepool.createfreethreaded)
- [Screen capture：帧尺寸、资源释放与重建](https://learn.microsoft.com/en-us/windows/apps/develop/media-authoring-processing/screen-capture)
- [GetModuleHandleExW：将实现模块固定到进程退出](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-getmodulehandleexw)
- [WGC 注销 COM 时提前卸载模块的同类问题](https://github.com/robmikh/Win32CaptureSample/issues/99)
