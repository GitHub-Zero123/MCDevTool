#pragma once
#include <vector>
#include <string_view>
#include <cstdint>
#include <optional>
#include <filesystem>

namespace MCDevTool::Level {
    // 实验性功能选项
    struct ExperimentsOptions {
        bool enable = false;
        bool dataDrivenBiomes = false;    // 数据驱动生物群系
        bool dataDrivenItems = false;     // 其他数据型驱动功能
        bool experimentalMolangFeatures = false; // 实验性Molang特性
    };

    // 存档基础选项
    struct LevelOptions {
        std::optional<uint64_t> seed = std::nullopt;            // 随机种子id
        uint32_t worldType = 1;                                 // 0-旧版有限世界 1-无限世界 2-超平坦
        uint32_t gameMode = 1;                                  // 0-生存 1-创造 2-冒险
        bool enableCheats = true;                               // 是否启用作弊
        bool keepInventory = true;                              // 死亡时是否保留物品栏
        bool doWeatherCycle = true;                             // 是否启用天气自然变化
        ExperimentsOptions experimentsOptions = {};     // 实验性功能选项
    };

    // 更新level.dat数据中的世界选项
    void updateLevelDatWorldData(std::vector<uint8_t>& levelDatData, std::optional<std::string_view> worldName, const LevelOptions& options={});

    // 创建一个默认存档level.dat数据
    std::vector<uint8_t> createDefaultLevelDat(
        std::string_view worldName, // 世界名称
        const LevelOptions& options={}
    );

    // 更新level.dat时间轴为当前时间
    void updateLevelDatLastPlayed(std::vector<uint8_t>& levelDatData);

    // 更新文件level.dat的时间轴并立即保存
    void updateLevelDatLastPlayedInFile(const std::filesystem::path& filePath);

    // 更新文件level.dat的世界数据选项并立即保存
    void updateLevelDatWorldDataInFile(
        const std::filesystem::path& filePath,
        std::optional<std::string_view> worldName,
        const LevelOptions& options={}
    );

    // 获取level.dat模板
    std::vector<uint8_t>& getLevelDatTemplate();
} // namespace MCDevTool::Level