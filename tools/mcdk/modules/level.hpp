#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <mcdevtool/level.h>
#include <mcdk_core/launch_config.h>

namespace mcdk {

    // 基于强类型 LaunchConfig 生成 LevelOptions（v2：原吃 const nlohmann::json& 已改为强类型嵌套 world 组）
    inline MCDevTool::Level::LevelOptions parseLevelOptionsFromUserConfig(const mcdk::core::LaunchConfig& config) {
        const auto& world = config.world;
        MCDevTool::Level::LevelOptions options;
        options.worldType = static_cast<uint32_t>(world.type);
        options.gameMode  = static_cast<uint32_t>(world.gameMode);
        if (world.seed.has_value()) {
            options.seed = world.seed.value();
        }
        options.enableCheats    = world.enableCheats;
        options.keepInventory   = world.keepInventory;
        options.doWeatherCycle  = world.doWeatherCycle;
        options.doDaylightCycle = world.doDaylightCycle;
        // 实验性玩法：存在 experiment_options 即启用（存在性即语义，对应原 contains+is_object 判定）
        if (world.experiments.has_value()) {
            MCDevTool::Level::ExperimentsOptions expOptions;
            expOptions.enable                     = true;
            expOptions.dataDrivenBiomes           = world.experiments->dataDrivenBiomes;
            expOptions.dataDrivenItems            = world.experiments->dataDrivenItems;
            expOptions.experimentalMolangFeatures = world.experiments->experimentalMolangFeatures;
            options.experimentsOptions            = expOptions;
        }
        return options;
    }

    // 根据强类型 LaunchConfig 创建 level.dat
    inline std::vector<uint8_t> createUserLevel(const mcdk::core::LaunchConfig& config) {
        auto        options   = parseLevelOptionsFromUserConfig(config);
        std::string worldName = config.world.name;
        auto        levelDat  = MCDevTool::Level::createDefaultLevelDat(worldName, options);
        return levelDat;
    }

} // namespace mcdk
