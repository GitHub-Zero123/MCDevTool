#pragma once

#include <cstdint>
#include <vector>
#include <functional>
#include <nlohmann/json.hpp>
#include <mcdevtool/style.h>

#include "console.hpp"

namespace mcdk {

    // 用户样式处理类
    class UserStyleProcessor : public MCDevTool::Style::MinecraftWindowStyler {
    public:
        UserStyleProcessor(int pid, const nlohmann::json& userConfig) : MCDevTool::Style::MinecraftWindowStyler(pid) {
            // 解析用户配置
            // 检查是否存在 window_style 字段
            if (!userConfig.contains("window_style")) {
                return;
            }
            auto&                         styleConfig = userConfig["window_style"];
            MCDevTool::Style::StyleConfig config;
            config.alwaysOnTop  = styleConfig.value("always_on_top", false);
            config.hideTitleBar = styleConfig.value("hide_title_bar", false);
            if (config.hideTitleBar || config.alwaysOnTop) {
                mNeedUpdateStyle = true;
            }
            // 处理 title_bar_color
            if (styleConfig.contains("title_bar_color") && styleConfig["title_bar_color"].is_array()
                && styleConfig["title_bar_color"].size() >= 3) {
                auto colorArray      = styleConfig["title_bar_color"];
                config.titleBarColor = std::vector<uint8_t>{
                    colorArray[0].get<uint8_t>(),
                    colorArray[1].get<uint8_t>(),
                    colorArray[2].get<uint8_t>()
                };
                mNeedUpdateStyle = true;
            }
            // 处理 fixed_size（支持大分辨率）
            if (styleConfig.contains("fixed_size") && styleConfig["fixed_size"].is_array()
                && styleConfig["fixed_size"].size() >= 2) {
                auto sizeArray   = styleConfig["fixed_size"];
                config.fixedSize = std::vector<int>{sizeArray[0].get<int>(), sizeArray[1].get<int>()};
                mNeedUpdateStyle = true;
            }
            // 处理 fixed_position
            if (styleConfig.contains("fixed_position") && styleConfig["fixed_position"].is_array()
                && styleConfig["fixed_position"].size() >= 2) {
                auto posArray        = styleConfig["fixed_position"];
                config.fixedPosition = std::vector<int>{posArray[0].get<int>(), posArray[1].get<int>()};
                mNeedUpdateStyle     = true;
            }
            // 处理 lock_corner
            if (styleConfig.contains("lock_corner") && styleConfig["lock_corner"].is_number_integer()) {
                config.lockCorner = styleConfig["lock_corner"].get<int>();
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
