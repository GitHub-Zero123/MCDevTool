#pragma once

#include <mcdevtool/style.h>

#include "console.hpp"

namespace mcdk {

    class UserStyleProcessor : public MCDevTool::Style::MinecraftWindowStyler {
    public:
        UserStyleProcessor(int pid, MCDevTool::Style::StyleConfig config);
        // Stop the worker while the derived output callback is still alive.
        ~UserStyleProcessor() override { safeExit(); }

        void setOutputCallback(ConsoleOutputCallback callback);
        void start();
        void onStyleApplied() override;

        [[nodiscard]] bool needUpdateStyle() const;

    private:
        bool                  mNeedUpdateStyle = false;
        ConsoleOutputCallback mOutputCallback;
    };

} // namespace mcdk
