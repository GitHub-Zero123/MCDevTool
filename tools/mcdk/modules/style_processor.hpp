#pragma once

#include <cstdint>
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>
#include <mcdevtool/style.h>
#include <mcdk_core/launch_config.h>

#include "console.hpp"

namespace mcdk {

    // 用户样式处理类
    class UserStyleProcessor : public MCDevTool::Style::MinecraftWindowStyler {
    public:
        // v2：原吃 const nlohmann::json& userConfig（内部取 window_style 段），已改为吃强类型 WindowStyle。
        UserStyleProcessor(int pid, const mcdk::core::LaunchConfig::WindowStyle& styleConfig)
        : MCDevTool::Style::MinecraftWindowStyler(pid) {
            MCDevTool::Style::StyleConfig config;
            config.alwaysOnTop  = styleConfig.alwaysOnTop;
            config.hideTitleBar = styleConfig.hideTitleBar;
            if (config.hideTitleBar || config.alwaysOnTop) {
                mNeedUpdateStyle = true;
            }
            // 处理 title_bar_color
            if (styleConfig.titleBarColor.has_value()) {
                const auto& colorArray = *styleConfig.titleBarColor;
                config.titleBarColor   = std::vector<uint8_t>{colorArray[0], colorArray[1], colorArray[2]};
                mNeedUpdateStyle       = true;
            }
            // 处理 fixed_size（支持大分辨率）
            if (styleConfig.fixedSize.has_value()) {
                const auto& sizeArray = *styleConfig.fixedSize;
                config.fixedSize      = std::vector<int>{sizeArray[0], sizeArray[1]};
                mNeedUpdateStyle      = true;
            }
            // 处理 fixed_position
            if (styleConfig.fixedPosition.has_value()) {
                const auto& posArray = *styleConfig.fixedPosition;
                config.fixedPosition = std::vector<int>{posArray[0], posArray[1]};
                mNeedUpdateStyle     = true;
            }
            // 处理 lock_corner
            if (styleConfig.lockCorner.has_value()) {
                config.lockCorner = *styleConfig.lockCorner;
                mNeedUpdateStyle  = true;
            }
            mConfig = std::move(config);
        }

        // 设置控制台输出回调
        void setOutputCallback(ConsoleOutputCallback callback) { mOutputCallback = std::move(callback); }

        void start() {
            if (mNeedUpdateStyle) {
                MinecraftWindowStyler::start();
                if (mOutputCallback) {
                    mOutputCallback("[Style] 已启用自定义样式，等待更新窗口中。", mcdk::ConsoleColor::Cyan);
                }
            }
        }

        void onStyleApplied() override {
            if (mOutputCallback) {
                mOutputCallback("[Style] 窗口样式更新已应用。", mcdk::ConsoleColor::Cyan);
            }
        }

        bool needUpdateStyle() const { return mNeedUpdateStyle; }

    private:
        bool                  mNeedUpdateStyle = false;
        ConsoleOutputCallback mOutputCallback;
    };

} // namespace mcdk
