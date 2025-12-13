#pragma once
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <cstdint>
#include <filesystem>

namespace MCDevTool::Addon {
    enum class PackType {
        BEHAVIOR,
        RESOURCE,
        UNKNOWN
    };

    struct PackInfo {
        std::string name;
        std::string uuid;
        std::string version;
        std::filesystem::path path;
        PackType type = PackType::UNKNOWN;

        explicit operator bool() const {
            return !uuid.empty() && !version.empty() && type != PackType::UNKNOWN;
        }
    };

    struct NeteasePackInfo {
        PackInfo baseInfo;
        std::vector<std::string> dependencies;

        explicit operator bool() const {
            return static_cast<bool>(baseInfo);
        }
    };

    // 根据JSON内容解析pack信息
    void parseJsonPackInfo(std::string_view jsonContent, PackInfo& out);

    // 根据路径解析pack
    PackInfo parsePackInfo(const std::filesystem::path& packPath);

    // 根据路径解析网易pack
    NeteasePackInfo parseNeteasePackInfo(const std::filesystem::path& packPath);

    // 创建空的AddonManifest 返回行为包与资源包的manifest字符串
    std::pair<std::string, std::string> createEmptyAddonManifest(
        const std::string& name="", const std::vector<uint32_t>& version = {1, 0, 0});
} // namespace MCDevTool::Addon