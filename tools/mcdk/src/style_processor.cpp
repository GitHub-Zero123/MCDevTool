#include <mcdk/style_processor.hpp>

#include <utility>

namespace mcdk {

    UserStyleProcessor::UserStyleProcessor(int pid, MCDevTool::Style::StyleConfig config)
    : MCDevTool::Style::MinecraftWindowStyler(pid, std::move(config)) {
        mNeedUpdateStyle = mConfig.alwaysOnTop || mConfig.hideTitleBar || mConfig.hideTaskbarIcon
                        || mConfig.titleBarColor.has_value() || mConfig.windowOpacity.has_value()
                        || mConfig.fixedSize.has_value() || mConfig.fixedPosition.has_value()
                        || mConfig.lockCorner.has_value();
    }

    void UserStyleProcessor::setOutputCallback(ConsoleOutputCallback callback) {
        mOutputCallback = std::move(callback);
    }

    void UserStyleProcessor::start() {
        if (!mNeedUpdateStyle) {
            return;
        }
        MinecraftWindowStyler::start();
        if (mOutputCallback) {
            mOutputCallback("[Style] 已启用自定义样式，等待更新窗口中。", ConsoleColor::Cyan);
        }
    }

    void UserStyleProcessor::onStyleApplied() {
        if (mOutputCallback) {
            mOutputCallback("[Style] 窗口样式更新已应用。", ConsoleColor::Cyan);
        }
    }

    bool UserStyleProcessor::needUpdateStyle() const { return mNeedUpdateStyle; }

} // namespace mcdk
