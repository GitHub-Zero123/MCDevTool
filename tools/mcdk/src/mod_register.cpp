#include <mcdk/mod_register.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include <INCLUDE_MOD.h>
#include <mcdevtool/addon.h>
#include <mcdevtool/env.h>

namespace mcdk {
    MCDevTool::Addon::PackInfo registerDebugMod() {
        using namespace MCDevTool;

        const auto        manifest = INCLUDE_MOD_RES::resourceMap.at("manifest.json");
        const std::string manifestContent(reinterpret_cast<const char*>(manifest.first), manifest.second);
        Addon::PackInfo   info;
        parseJsonPackInfo(manifestContent, info);

        std::filesystem::path outputDirectory;
        if (info.type == Addon::PackType::BEHAVIOR) {
            outputDirectory = getBehaviorPacksPath();
        } else if (info.type == Addon::PackType::RESOURCE) {
            outputDirectory = getResourcePacksPath();
        } else {
            throw std::runtime_error("调试MOD的PackType类型未知，无法注册。");
        }

        auto uuidWithoutDashes = info.uuid;
        std::erase(uuidWithoutDashes, '-');
        const auto target = outputDirectory / uuidWithoutDashes;
        if (std::filesystem::exists(target)) {
            std::filesystem::remove_all(target);
        }

        for (const auto& [resourceName, resourceData] : INCLUDE_MOD_RES::resourceMap) {
            const auto resourcePath = target / std::filesystem::u8path(resourceName);
            std::filesystem::create_directories(resourcePath.parent_path());
            std::ofstream output(resourcePath, std::ios::binary);
            output.write(
                reinterpret_cast<const char*>(resourceData.first),
                static_cast<std::streamsize>(resourceData.second)
            );
        }
        return info;
    }

    void linkUserConfigModDirs(
        std::vector<UserModDirConfig>&           configs,
        std::vector<MCDevTool::Addon::PackInfo>& linkedPacks,
        bool                                     updateConfigPaths
    ) {
        using namespace MCDevTool;

        for (auto& modConfig : configs) {
            auto packInfos = linkSourceAddonToRuntimePacks(modConfig.getAbsolutePath());
            for (auto& info : packInfos) {
                if (info.type == Addon::PackType::BEHAVIOR) {
                    std::cout << "  Behavior \"" << info.name << "\"  UUID=" << info.uuid;
                    if (modConfig.hotReload) {
                        std::cout << "  Watch=Py";
                        if (updateConfigPaths) {
                            modConfig.path = info.path;
                        }
                    }
                    std::cout << '\n';
                } else if (info.type == Addon::PackType::RESOURCE) {
                    std::cout << "  Resource \"" << info.name << "\"  UUID=" << info.uuid << '\n';
                }
                linkedPacks.push_back(std::move(info));
            }
        }
    }

} // namespace mcdk
