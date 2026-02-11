#pragma once
#include <iostream>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include <INCLUDE_MOD.h>
#include <mcdevtool/env.h>
#include <mcdevtool/addon.h>

#include "mod_dir_config.hpp"
#include "utils.hpp"

namespace mcdk {

    // 注册调试MOD
    inline MCDevTool::Addon::PackInfo registerDebugMod(
        const nlohmann::json&                config,
        const std::vector<UserModDirConfig>& modDirConfigs,
        std::filesystem::path*               outConfigFile = nullptr
    ) {
        using namespace MCDevTool;

        auto            manifest = INCLUDE_MOD_RES::resourceMap.at("manifest.json");
        std::string     manifestContent(reinterpret_cast<const char*>(manifest.first), manifest.second);
        Addon::PackInfo info;
        parseJsonPackInfo(manifestContent, info);
        std::filesystem::path outDir;

        if (info.type == Addon::PackType::BEHAVIOR) {
            outDir = getBehaviorPacksPath();
        } else if (info.type == Addon::PackType::RESOURCE) {
            outDir = getResourcePacksPath();
        } else {
            throw std::runtime_error("调试MOD的PackType类型未知，无法注册。");
        }

        std::string uuidNoDash = info.uuid;
        uuidNoDash.erase(std::remove(uuidNoDash.begin(), uuidNoDash.end(), '-'), uuidNoDash.end());
        auto target = outDir / uuidNoDash;

        // 写入ADDON数据到目标目录
        if (std::filesystem::exists(target)) {
            std::filesystem::remove_all(target);
        }

        // 处理debug_options参数
        auto configDebugOptions = config.value("debug_options", nlohmann::json::object());

        // 生成格式化的字面量json
        auto DEBUG_OPTIONS = configDebugOptions.dump();

        // 替换为python boolean格式
        stringReplace(DEBUG_OPTIONS, "true", "True");
        stringReplace(DEBUG_OPTIONS, "false", "False");
        stringReplace(DEBUG_OPTIONS, "null", "None");

        for (const auto& [resName, resData] : INCLUDE_MOD_RES::resourceMap) {
            auto resPath = target / std::filesystem::u8path(resName);
            std::filesystem::create_directories(resPath.parent_path());
            std::ofstream resFile(resPath, std::ios::binary);
            if (resName.ends_with("Config.py")) {
                // 替换关键字实现传参
                std::string content(reinterpret_cast<const char*>(resData.first), resData.second);
                stringReplace(content, "\"{#debug_options}\"", DEBUG_OPTIONS);
                stringReplace(
                    content,
                    "\"{#target_mod_dirs}\"",
                    UserModDirConfig::toHotReloadListString(modDirConfigs)
                );
                resFile.write(content.data(), content.size());
                if (outConfigFile != nullptr) {
                    outConfigFile->assign(resPath);
                }
            } else {
                resFile.write(reinterpret_cast<const char*>(resData.first), resData.second);
            }
            resFile.close();
        }

        return info;
    }

    // link用户mod目录 基于配置结构体
    inline void linkUserConfigModDirs(
        std::vector<UserModDirConfig>&           configs,
        std::vector<MCDevTool::Addon::PackInfo>& linkedPacks,
        bool                                     updateConfigPaths = false
    ) {
        using namespace MCDevTool;

        for (auto& modConfig : configs) {
            auto dir       = modConfig.getAbsolutePath();
            auto packInfos = linkSourceAddonToRuntimePacks(dir);
            for (const auto& info : packInfos) {
                if (info.type == Addon::PackType::BEHAVIOR) {
                    std::cout << "[MCDK] LINK行为包: \"" << info.name << "\", UUID: " << info.uuid << "\n";
                    if (modConfig.hotReload) {
                        std::cout << "  └── 热更新标记追踪\n";
                        if (updateConfigPaths) {
                            modConfig.path = info.path; // 重新更新为link后的路径
                        }
                    }
                } else if (info.type == Addon::PackType::RESOURCE) {
                    std::cout << "[MCDK] LINK资源包: \"" << info.name << "\", UUID: " << info.uuid << "\n";
                }
                linkedPacks.push_back(std::move(info));
            }
        }
    }

} // namespace mcdk
