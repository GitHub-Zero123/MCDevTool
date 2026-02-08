#pragma once
#include <iostream>
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include <mcdevtool/env.h>

namespace mcdk {
    // 创建默认配置
    inline nlohmann::json createDefaultConfig() {
        std::filesystem::path exePath;
        auto                  autoExePath = MCDevTool::autoMatchLatestGameExePath();
        if (!autoExePath.has_value()) {
            // 无法自动匹配到游戏exe路径，要求用户输入
            std::string u8input;
            std::cout << "请输入游戏可执行文件路径：";
            std::getline(std::cin, u8input);
            if (u8input.size() > 2 && u8input[0] == '"' && u8input[u8input.size() - 1] == '"') {
                // 针对字符串形式路径解析
                u8input = u8input.substr(1, u8input.size() - 2);
            }
            exePath = std::filesystem::u8path(u8input);
        } else {
            exePath = autoExePath.value();
        }
        if (!std::filesystem::is_regular_file(exePath)) {
            std::cerr << "路径无效，文件不存在。\n";
            exit(1);
        }
        nlohmann::json config{
            // 包含的mod目录，支持多个
            {   "included_mod_dirs", nlohmann::json::array({"./"})},
            // 世界随机种子 留空则随机生成
            {          "world_seed",                       nullptr},
            // 是否重置世界
            {         "reset_world",                         false},
            // 世界名称
            {          "world_name",                "MC_DEV_WORLD"},
            // 世界文件夹名称
            {   "world_folder_name",                "MC_DEV_WORLD"},
            // 是否自动进入游戏存档
            {      "auto_join_game",                          true},
            // 包含调试模组(提供R键热更新以及py输出流标记)
            {   "include_debug_mod",                          true},
            // 是否启用自动热更新mod功能
            {"auto_hot_reload_mods",                          true},
            // 世界类型 0-旧版 1-无限 2-平坦
            {          "world_type",                             1},
            // 游戏模式 0-生存 1-创造 2-冒险
            {           "game_mode",                             1},
            // 是否启用作弊
            {       "enable_cheats",                          true},
            // 保留物品栏
            {      "keep_inventory",                          true},
        };
        // 游戏可执行文件路径
        auto u8Path = exePath.generic_u8string();
        if constexpr (sizeof(char8_t) == sizeof(char)) {
            config["game_executable_path"] = reinterpret_cast<const char*>(u8Path.c_str());
        } else {
            config["game_executable_path"] = std::string(u8Path.begin(), u8Path.end());
        }
        return config;
    }

    // 解析用户配置文件
    inline nlohmann::json userParseConfig() {
        nlohmann::json config;
        auto           configPath = std::filesystem::current_path() / ".mcdev.json";
        if (!std::filesystem::is_regular_file(configPath)) {
            // 初始化默认配置文件
            config = createDefaultConfig();
            std::ofstream configFile(configPath);
            configFile << config.dump(4);
            configFile.close();
        } else {
            std::ifstream configFile(configPath, std::ios::binary);
            std::string   content((std::istreambuf_iterator<char>(configFile)), std::istreambuf_iterator<char>());
            // 启用注释支持
            config = nlohmann::json::parse(content, nullptr, false, true);
            configFile.close();
            if (config.is_discarded()) {
                throw std::runtime_error("配置文件解析失败，JSON异常，请检查格式是否正确。");
            }
        }
        return config;
    }

    // 尝试更新游戏路径
    inline bool updateGamePath(std::filesystem::path& path) {
        auto autoExePath = MCDevTool::autoMatchLatestGameExePath();
        if (autoExePath.has_value()) {
            path = std::move(autoExePath.value());
            return true;
        }
        return false;
    }

    // 尝试更新用户游戏路径配置
    inline void tryUpdateUserGamePath(const std::filesystem::path& newPath) {
        auto           configPath = std::filesystem::current_path() / ".mcdev.json";
        std::ifstream  configFile(configPath, std::ios::binary);
        std::string    content((std::istreambuf_iterator<char>(configFile)), std::istreambuf_iterator<char>());
        nlohmann::json config = nlohmann::json::parse(content, nullptr, false, true);
        configFile.close();
        if (config.is_discarded()) {
            throw std::runtime_error("配置文件解析失败，JSON异常，请检查格式是否正确。");
        }
        auto u8Path = newPath.generic_u8string();
        if constexpr (sizeof(char8_t) == sizeof(char)) {
            config["game_executable_path"] = reinterpret_cast<const char*>(u8Path.c_str());
        } else {
            config["game_executable_path"] = std::string(u8Path.begin(), u8Path.end());
        }
        // 重新写入配置文件
        std::ofstream outConfigFile(configPath, std::ios::binary | std::ios::trunc);
        outConfigFile << config.dump(4);
        outConfigFile.close();
    }

} // namespace mcdk
