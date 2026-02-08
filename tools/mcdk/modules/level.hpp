#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <mcdevtool/level.h>

namespace mcdk {

    // 基于用户config生成LevelOptions数据
    inline MCDevTool::Level::LevelOptions parseLevelOptionsFromUserConfig(const nlohmann::json& config) {
        MCDevTool::Level::LevelOptions options;
        options.worldType = static_cast<uint32_t>(config.value("world_type", 1));
        options.gameMode  = static_cast<uint32_t>(config.value("game_mode", 1));
        if (config.contains("world_seed") && !config["world_seed"].is_null()) {
            options.seed = config["world_seed"].get<uint64_t>();
        }
        options.enableCheats    = config.value("enable_cheats", true);
        options.keepInventory   = config.value("keep_inventory", true);
        options.doWeatherCycle  = config.value("do_weather_cycle", true);
        options.doDaylightCycle = config.value("do_daylight_cycle", true);
        // 处理实验性选项
        MCDevTool::Level::ExperimentsOptions expOptions;
        // 检查存在experiment_options字段
        if (config.contains("experiment_options")) {
            auto experimentOptions = config["experiment_options"];
            // 实验性玩法参数处理
            if (experimentOptions.is_object()) {
                expOptions.enable                     = true;
                expOptions.dataDrivenBiomes           = experimentOptions.value("data_driven_biomes", false);
                expOptions.dataDrivenItems            = experimentOptions.value("data_driven_items", false);
                expOptions.experimentalMolangFeatures = experimentOptions.value("experimental_molang_features", false);
                options.experimentsOptions            = expOptions;
            }
        }
        return options;
    }

    // 根据用户config创建level.dat
    inline std::vector<uint8_t> createUserLevel(const nlohmann::json& config) {
        auto        options   = parseLevelOptionsFromUserConfig(config);
        std::string worldName = config.value("world_name", "MC_DEV_WORLD");
        auto        levelDat  = MCDevTool::Level::createDefaultLevelDat(worldName, options);
        return levelDat;
    }

} // namespace mcdk
