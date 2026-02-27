#pragma once
#include <string>
#include <vector>
#include <optional>
#include <thread>
#include <atomic>
#include <cstdint>

namespace MCDevTool::Style {
    struct StyleConfig {
        // 悬浮置顶
        bool alwaysOnTop = false;
        // 隐藏标题栏
        bool hideTitleBar = false;
        // 自定义标题栏颜色 null | int[R,G,B] (0-255)
        std::optional<std::vector<uint8_t>> titleBarColor = std::nullopt; // RGB
        // 锁定大小 null | int[w, h]
        std::optional<std::vector<int>> fixedSize = std::nullopt; // [w, h]
        // 锁定屏幕位置 null | int[x, y]
        std::optional<std::vector<int>> fixedPosition = std::nullopt; // [x, y]
        // 锁定在屏幕四个脚落（覆盖fixed_position）1. 左上 2. 右上 3. 左下 4. 右下 null | int
        std::optional<int> lockCorner = std::nullopt;
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
        virtual ~MinecraftWindowStyler() = default;

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

    // 根据指定pid获取窗口内的画面信息 返回压缩480p的jpg数据
    std::optional<std::vector<uint8_t>> captureMinecraftWindow480p(int pid);

    // 根据指定pid点击窗口画面的指定坐标（百分比为单位确保适配不同分辨率）点击坐标为(0.0-1.0, 0.0-1.0)
    bool clickMinecraftWindowAt(int pid, double xPercent, double yPercent);
} // namespace MCDevTool::Style