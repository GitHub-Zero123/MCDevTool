#include <mod_register.hpp>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <type_traits>

#include <INCLUDE_MOD.h>
#include <mcdevtool/addon.h>
#include <mcdevtool/env.h>
#include <nlohmann/json.hpp>

#include <utils.hpp>

namespace mcdk {
    namespace {
        nlohmann::json debugOptionToJson(const DebugOptionValue& option) {
            return std::visit(
                [](const auto& value) -> nlohmann::json {
                    using Value = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Value, DebugOptionValue::Array>) {
                        auto result = nlohmann::json::array();
                        for (const auto& item : value) {
                            result.push_back(debugOptionToJson(item));
                        }
                        return result;
                    } else if constexpr (std::is_same_v<Value, DebugOptionValue::Object>) {
                        auto result = nlohmann::json::object();
                        for (const auto& [key, item] : value) {
                            result[key] = debugOptionToJson(item);
                        }
                        return result;
                    } else {
                        return value;
                    }
                },
                option.value
            );
        }

        std::string toPythonLiteral(const DebugModOptions& options) {
            auto json = nlohmann::json::object();
            for (const auto& [key, value] : options.additionalOptions) {
                json[key] = debugOptionToJson(value);
            }
            if (options.reloadKey) {
                json["reload_key"] = debugOptionToJson(*options.reloadKey);
            }
            if (options.reloadWorldKey) {
                json["reload_world_key"] = debugOptionToJson(*options.reloadWorldKey);
            }
            if (options.reloadAddonKey) {
                json["reload_addon_key"] = debugOptionToJson(*options.reloadAddonKey);
            }
            if (options.reloadShadersKey) {
                json["reload_shaders_key"] = debugOptionToJson(*options.reloadShadersKey);
            }
            if (options.reloadKeyGlobal) {
                json["reload_key_global"] = debugOptionToJson(*options.reloadKeyGlobal);
            }

            auto literal = json.dump();
            stringReplace(literal, "true", "True");
            stringReplace(literal, "false", "False");
            stringReplace(literal, "null", "None");
            return literal;
        }
    } // namespace

    MCDevTool::Addon::PackInfo registerDebugMod(
        const DebugModOptions&               options,
        const std::vector<UserModDirConfig>& modDirectories,
        std::filesystem::path*               outConfigFile
    ) {
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

        const auto debugOptions = toPythonLiteral(options);
        for (const auto& [resourceName, resourceData] : INCLUDE_MOD_RES::resourceMap) {
            const auto resourcePath = target / std::filesystem::u8path(resourceName);
            std::filesystem::create_directories(resourcePath.parent_path());
            std::ofstream output(resourcePath, std::ios::binary);
            if (resourceName.ends_with("Config.py")) {
                std::string content(reinterpret_cast<const char*>(resourceData.first), resourceData.second);
                stringReplace(content, "\"{#debug_options}\"", debugOptions);
                stringReplace(
                    content,
                    "\"{#target_mod_dirs}\"",
                    UserModDirConfig::toHotReloadListString(modDirectories)
                );
                output.write(content.data(), static_cast<std::streamsize>(content.size()));
                if (outConfigFile != nullptr) {
                    *outConfigFile = resourcePath;
                }
            } else {
                output.write(
                    reinterpret_cast<const char*>(resourceData.first),
                    static_cast<std::streamsize>(resourceData.second)
                );
            }
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
