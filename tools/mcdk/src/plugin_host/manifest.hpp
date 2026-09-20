#pragma once
// plugin.json 清单的解析。格式见 docs/plugin-system/06-loading.md §3。
// 清单存在的理由不是「把元数据写两遍」，而是让宿主在 **LoadLibrary 之前** 就知道
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <mcdk/plugin/abi/core.h>

namespace mcdk::plugin_host::detail {

    struct PluginDependency {
        std::string id;
        // ">=0.3.0" / ">1.0" / "=2.1.0" / "*" / 空。空与 "*" 都表示不限版本。
        std::string versionSpec;
    };

    struct PluginManifest {
        int                   schema = 1;
        std::string           id;
        std::string           name;
        std::string           version;
        std::string           entrySymbol;
        std::string           description;
        std::uint32_t         abiMajor = 0;
        std::uint32_t         abiMinor = 0;
        mcdk_stage            minStage = MCDK_STAGE_REGISTER;
        // 已按当前平台选中并解析成的绝对路径。
        std::filesystem::path libraryPath;
        std::vector<PluginDependency>  dependencies;
        std::vector<std::string>       permissions;
    };

    // 读取 directory/plugin.json。失败时返回 nullopt 并填 error。
    [[nodiscard]] std::optional<PluginManifest>
    readManifest(const std::filesystem::path& directory, std::string& error);

    // 版本比较，用于依赖的 versionSpec 判定。
    // 版本串按点分整数比较，缺位补 0；非数字段落忽略其后部分（"1.0.0-beta" 视为 1.0.0）。
    [[nodiscard]] bool versionSatisfies(const std::string& version, const std::string& spec);

} // namespace mcdk::plugin_host::detail
