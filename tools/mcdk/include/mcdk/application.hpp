#pragma once

#include <mcdk/game_process.hpp>
#include <mcdk/settings.hpp>

namespace mcdk {

    GameRunResult startGame(const UserConfig& config);

    // 加载配置声明的插件后 startGame，无论怎样退出都走完插件终结。
    GameRunResult runGame(const UserConfig& config);

} // namespace mcdk
