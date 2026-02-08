#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace mcdk {

    // 用户mod目录配置结构体
    class UserModDirConfig {
    public:
        std::filesystem::path path;
        bool                  hotReload = false;

        UserModDirConfig() = default;
        explicit UserModDirConfig(const std::filesystem::path& p, bool hr) : path(p), hotReload(hr) {}

        // 获取基于工作区的绝对路径
        std::filesystem::path getAbsolutePath() const {
            static const auto workingDir = std::filesystem::current_path();
            if (path.is_absolute()) {
                return path.lexically_normal();
            } else {
                return (workingDir / path).lexically_normal();
            }
        }

        // 获取绝对路径的utf8 linux风格字符串
        std::string getAbsoluteU8String() const {
            auto absPath = getAbsolutePath().generic_u8string();
            if constexpr (sizeof(char8_t) == sizeof(char)) {
                return reinterpret_cast<const char*>(absPath.c_str());
            } else {
                return std::string(absPath.begin(), absPath.end());
            }
        }

        // 从json对象解析UserModDirConfig
        static UserModDirConfig parseFromJson(const nlohmann::json& j) {
            UserModDirConfig config;
            if (j.is_string()) {
                config.path      = std::filesystem::u8path(j.get<std::string>());
                config.hotReload = true;
            } else if (j.is_object()) {
                config.path      = std::filesystem::u8path(j.value("path", "./"));
                config.hotReload = j.value("hot_reload", true);
            } else {
                throw std::runtime_error("Invalid mod directory configuration format.");
            }
            return config;
        }

        // 从json数组解析UserModDirConfig列表
        static std::vector<UserModDirConfig> parseListFromJson(const nlohmann::json& jArray) {
            std::vector<UserModDirConfig> configs;
            if (!jArray.is_array()) {
                throw std::runtime_error("Mod directories configuration should be an array.");
            }
            for (const auto& item : jArray) {
                configs.push_back(parseFromJson(item));
            }
            return configs;
        }

        // 基于std::vector<std::string>构造UserModDirConfig列表
        static std::vector<UserModDirConfig> fromStringList(const std::vector<std::string>& u8Paths) {
            std::vector<UserModDirConfig> configs;
            for (const auto& u8Path : u8Paths) {
                configs.emplace_back(std::filesystem::u8path(u8Path), true);
            }
            return configs;
        }

        // 将一组vector<UserModDirConfig>生成字符串形式的hot_reload追踪列表（json数组格式）
        static std::string toHotReloadListString(const std::vector<UserModDirConfig>& configs) {
            nlohmann::json jArray = nlohmann::json::array();
            for (const auto& config : configs) {
                if (config.hotReload) {
                    jArray.push_back(config.getAbsoluteU8String());
                }
            }
            return jArray.dump();
        }

        // 将std::vector<UserModDirConfig>转换为std::vector<std::filesystem::path>的目标追踪列表
        static std::vector<std::filesystem::path> toPathList(const std::vector<UserModDirConfig>& configs) {
            std::vector<std::filesystem::path> paths;
            for (const auto& config : configs) {
                if (config.hotReload) {
                    paths.push_back(config.getAbsolutePath());
                }
            }
            return paths;
        }
    };

} // namespace mcdk
