#pragma once

#include <mcdevtool/style.h>

#include "console.hpp"

namespace mcdk {

    class UserStyleProcessor : public MCDevTool::Style::MinecraftWindowStyler {
    public:
        UserStyleProcessor(int pid, MCDevTool::Style::StyleConfig config);

        void setOutputCallback(ConsoleOutputCallback callback);
        void start();
        void onStyleApplied() override;

        [[nodiscard]] bool needUpdateStyle() const;

    private:
        bool                  mNeedUpdateStyle = false;
        ConsoleOutputCallback mOutputCallback;
    };

} // namespace mcdk
