#include <mcdk/level.hpp>

#include <mcdevtool/level.h>

namespace mcdk {

    std::vector<uint8_t> createUserLevel(const WorldProjectConfig& config) {
        return MCDevTool::Level::createDefaultLevelDat(config.name, config.level);
    }

} // namespace mcdk
