#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <expected>
#include <optional>
#include <thread>
#include <atomic>
#include <cstdint>

namespace MCDevTool::Style {
    struct RgbColor {
        uint8_t red   = 0;
        uint8_t green = 0;
        uint8_t blue  = 0;

        bool operator==(const RgbColor&) const = default;
    };

    struct WindowSize {
        int width  = 0;
        int height = 0;

        bool operator==(const WindowSize&) const = default;
    };

    struct WindowPosition {
        int x = 0;
        int y = 0;

        bool operator==(const WindowPosition&) const = default;
    };

    enum class WindowCorner : int {
        TopLeft     = 1,
        TopRight    = 2,
        BottomLeft  = 3,
        BottomRight = 4,
    };

    struct StyleConfig {
        // 悬浮置顶
        bool alwaysOnTop = false;
        // 隐藏标题栏
        bool hideTitleBar = false;
        // 隐藏任务栏图标
        bool hideTaskbarIcon = false;
        // 自定义标题栏颜色 null | int[R,G,B] (0-255)
        std::optional<RgbColor> titleBarColor = std::nullopt;
        // 窗口整体不透明度 null | int (0-255)
        std::optional<uint8_t> windowOpacity = std::nullopt;
        // 锁定大小 null | int[w, h]
        std::optional<WindowSize> fixedSize = std::nullopt;
        // 锁定屏幕位置 null | int[x, y]
        std::optional<WindowPosition> fixedPosition = std::nullopt;
        // 锁定在屏幕四个脚落（覆盖fixed_position）1. 左上 2. 右上 3. 左下 4. 右下 null | int
        std::optional<WindowCorner> lockCorner = std::nullopt;
    };

    // 设置指定pid的Minecraft窗口样式
    bool applyStyleToMinecraftWindow(int pid, const StyleConfig& config);

    // MinecraftWindowStyler 类，用于持续应用样式
    class MinecraftWindowStyler {
    public:
        MinecraftWindowStyler(int pid, const StyleConfig& config);
        MinecraftWindowStyler(int pid, StyleConfig&& config);
        MinecraftWindowStyler(int pid);
        MinecraftWindowStyler()          = default;
        virtual ~MinecraftWindowStyler();

        virtual void onStyleApplied();

        void start();
        void safeExit();
        void join();

        void setPid(int pid);

    protected:
        int                        mPid;
        StyleConfig                mConfig;
        std::optional<std::thread> mThread;
        std::atomic<bool>          mStopFlag = false;
    };

    // 客户区内的截取范围，归一化到 0.0-1.0，与 mc_input 的坐标系同构：(0,0) 左上，
    // (1,1) 右下。默认整块客户区。
    struct CaptureRegion {
        double left   = 0.0;
        double top    = 0.0;
        double right  = 1.0;
        double bottom = 1.0;
    };

    struct CaptureOptions {
        // JPEG 高度上限，保持宽高比，小窗口不放大。
        unsigned maxHeight = 480;
        // 只截客户区里的这一块。配合 maxHeight 可以在不把整张图放大的前提下看清物品
        // 数量、tooltip、聊天这类小字——截一块比整张放大更清楚，数据量也更小。
        CaptureRegion region{};
    };

    enum class CaptureError {
        WindowNotFound,     // 按 pid 找不到游戏窗口：没启动、已退出，或 pid 过期
        WindowMinimized,    // 窗口最小化，没有可供捕获的画面
        CaptureUnavailable, // 系统不支持或禁用了窗口捕获，也包括锁屏 / 远程桌面断开
        Timeout,            // 会话已建立，但时限内没等到可用帧
        InvalidRegion,      // region 不是有效的归一化矩形
        Failed,             // 其他失败：设备丢失、编码失败等
    };

    // 面向调用方（含 MCP 错误文案）的说明，每条都带下一步该怎么做。
    std::string_view describeCaptureError(CaptureError error);

    // WGC 捕获客户区（支持遮挡），返回保持比例的 JPEG，含系统指针；不放大小窗口。
    // 需要 Windows 10 1809+。
    std::expected<std::vector<uint8_t>, CaptureError>
    captureMinecraftWindowJpeg(int pid, CaptureOptions options = {});


    // Trigger Minecraft's native Ctrl+R UI definition reload from the host process.
    bool triggerMinecraftUiReloadShortcut(int pid);
} // namespace MCDevTool::Style
