#pragma once
#include <vector>
#include <string_view>
#include <cstdint>
#include <optional>

namespace MCDevTool::Level {
    struct LevelOptions {
        std::optional<uint64_t> seed=std::nullopt;            // 随机种子id
        uint32_t worldType=1;                                 // 0-旧版有限世界 1-无限世界 2-超平坦
        uint32_t gameMode=1;                                  // 0-生存 1-创造 2-冒险
        bool enableCheats=true;                               // 是否启用作弊
        bool keepInventory=true;                              // 死亡时是否保留物品栏
    };

    // 创建一个默认存档level.dat数据
    std::vector<uint8_t> createDefaultLevelDat(
        std::string_view worldName, // 世界名称
        const LevelOptions& options={}
    );

    // 获取level.dat模板
    std::vector<uint8_t>& getLevelDatTemplate();
} // namespace MCDevTool::Level