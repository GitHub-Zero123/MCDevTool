// `mcdk plugin ...` 子命令。规范见 docs/plugin-system/06-loading.md §5。
// 这几条命令是信任模型（§1）里用户做决策的那一刻的载体：插件不会被自动发现，
#include <mcdk/plugin_cli.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include <mcdevtool/utils.h>

#include "plugin_host/manifest.hpp"

namespace mcdk {

    namespace {

        using Json = nlohmann::ordered_json;

        [[nodiscard]] std::filesystem::path configPath() { return std::filesystem::current_path() / ".mcdev.json"; }

        [[nodiscard]] Json readConfig() {
            const auto path = configPath();
            if (!std::filesystem::is_regular_file(path)) {
                return Json::object();
            }
            std::ifstream     input(path, std::ios::binary);
            const std::string content{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            auto              result = Json::parse(content, nullptr, false, /*ignore_comments=*/true);
            if (result.is_discarded()) {
                throw std::runtime_error(".mcdev.json 解析失败，请检查格式。");
            }
            return result;
        }
        // 注意：写回会丢掉原文件的注释和缩进（jsonc 解析不会保留它们）。
        void writeConfig(const Json& config) {
            const auto path = configPath();
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << config.dump(4) << '\n';
            std::cout << "已写入 " << MCDevTool::Utils::pathToGenericUtf8(path) << "\n";
            std::cout << "注意：写回会丢失原文件中的注释与缩进风格。\n";
        }

        [[nodiscard]] Json& pluginsArray(Json& config) {
            auto it = config.find("plugins");
            if (it == config.end() || !it->is_array()) {
                config["plugins"] = Json::array();
                it                = config.find("plugins");
            }
            return *it;
        }

        // 按 id 或 path 定位一条声明。两者都匹配不到时返回 nullptr。
        [[nodiscard]] Json* findDeclaration(Json& plugins, const std::string& key) {
            for (auto& item : plugins) {
                if (!item.is_object()) {
                    continue;
                }
                if (item.value("id", std::string{}) == key || item.value("path", std::string{}) == key) {
                    return &item;
                }
            }
            return nullptr;
        }

        void printManifestSummary(const plugin_host::detail::PluginManifest& manifest) {
            std::cout << "  id        " << manifest.id << "\n";
            if (!manifest.name.empty()) {
                std::cout << "  name      " << manifest.name << "\n";
            }
            std::cout << "  version   " << (manifest.version.empty() ? "(未声明)" : manifest.version) << "\n";
            std::cout << "  abi       " << manifest.abiMajor << "." << manifest.abiMinor << "\n";
            std::cout << "  library   " << manifest.libraryPath.generic_string() << "\n";
            if (!manifest.dependencies.empty()) {
                std::cout << "  依赖\n";
                for (const auto& dependency : manifest.dependencies) {
                    std::cout << "    - " << dependency.id
                              << (dependency.versionSpec.empty() ? "" : " " + dependency.versionSpec) << "\n";
                }
            }
            // 权限是用户做信任决策的依据，即使为空也要明说，不能省略这一行。
            std::cout << "  权限      ";
            if (manifest.permissions.empty()) {
                std::cout << "(未声明)\n";
            } else {
                for (std::size_t index = 0; index < manifest.permissions.size(); ++index) {
                    std::cout << (index == 0 ? "" : ", ") << manifest.permissions[index];
                }
                std::cout << "\n";
            }
        }

    } // namespace

    int pluginList() {
        auto        config  = readConfig();
        auto&       plugins = pluginsArray(config);
        const auto  base    = std::filesystem::current_path();

        if (plugins.empty()) {
            std::cout << ".mcdev.json 中没有任何插件声明。\n";
            std::cout << "用 `mcdk plugin add <path>` 添加。\n";
            return 0;
        }

        std::size_t index = 0;
        for (const auto& item : plugins) {
            const bool  enabled = item.value("enable", false);
            const auto  rawPath = item.value("path", std::string{});
            std::cout << "[" << index++ << "] " << (enabled ? "启用  " : "禁用  ") << rawPath << "\n";

            // 解析结果也一并展示：用户最想知道的是「这条声明到底指向什么」。
            const auto resolved = base / std::filesystem::u8path(rawPath);
            if (std::filesystem::is_directory(resolved)) {
                std::string error;
                if (const auto manifest = plugin_host::detail::readManifest(resolved, error)) {
                    printManifestSummary(*manifest);
                } else {
                    std::cout << "  ! " << error << "\n";
                }
            } else if (std::filesystem::is_regular_file(resolved)) {
                std::cout << "  直指动态库（无清单，仅建议本地调试用）\n";
            } else {
                std::cout << "  ! 路径不存在\n";
            }
            if (const auto declaredId = item.value("id", std::string{}); !declaredId.empty()) {
                std::cout << "  声明 id  " << declaredId << "\n";
            }
        }
        return 0;
    }

    int pluginSetEnabled(const std::string& key, bool enabled) {
        auto  config      = readConfig();
        auto& plugins     = pluginsArray(config);
        auto* declaration = findDeclaration(plugins, key);
        if (declaration == nullptr) {
            std::cerr << "找不到匹配 " << key << " 的声明（按 id 或 path 匹配）。\n";
            return 1;
        }
        if (declaration->value("enable", false) == enabled) {
            std::cout << key << " 已经是" << (enabled ? "启用" : "禁用") << "状态，未改动。\n";
            return 0;
        }
        (*declaration)["enable"] = enabled;
        writeConfig(config);
        return 0;
    }

    int pluginAdd(const std::string& rawPath) {
        const auto base     = std::filesystem::current_path();
        const auto resolved = std::filesystem::absolute(base / std::filesystem::u8path(rawPath));

        if (!std::filesystem::is_directory(resolved)) {
            std::cerr << "只能添加插件目录（内含 plugin.json）。直指动态库的形态请手写声明，"
                         "它没有清单可供展示，无法完成信任确认这一步。\n";
            return 1;
        }

        std::string error;
        const auto  manifest = plugin_host::detail::readManifest(resolved, error);
        if (!manifest) {
            std::cerr << "读取清单失败：" << error << "\n";
            return 1;
        }

        // §1 信任模型里用户做决策的那一刻：先看清楚是什么、要什么权限，再决定。
        std::cout << "即将添加插件：\n";
        printManifestSummary(*manifest);
        std::cout << "\n确认添加？[y/N] " << std::flush;
        std::string answer;
        std::getline(std::cin, answer);
        if (answer != "y" && answer != "Y") {
            std::cout << "已取消。\n";
            return 1;
        }

        auto  config  = readConfig();
        auto& plugins = pluginsArray(config);
        if (findDeclaration(plugins, manifest->id) != nullptr) {
            std::cerr << manifest->id << " 已在声明列表中。\n";
            return 1;
        }

        // 相对路径以 .mcdev.json 所在目录为基准，能转相对就转，便于项目整体移动。
        std::error_code ignored;
        auto            stored = std::filesystem::relative(resolved, base, ignored).generic_string();
        if (stored.empty() || stored.rfind("..", 0) == 0) {
            stored = resolved.generic_string();
        }

        Json entry        = Json::object();
        entry["enable"]   = true;
        entry["path"]     = stored;
        entry["id"]       = manifest->id;
        plugins.push_back(std::move(entry));
        writeConfig(config);
        return 0;
    }

    int pluginRemove(const std::string& key) {
        auto  config  = readConfig();
        auto& plugins = pluginsArray(config);
        const auto before = plugins.size();
        for (auto it = plugins.begin(); it != plugins.end();) {
            if (it->is_object()
                && (it->value("id", std::string{}) == key || it->value("path", std::string{}) == key)) {
                it = plugins.erase(it);
            } else {
                ++it;
            }
        }
        if (plugins.size() == before) {
            std::cerr << "找不到匹配 " << key << " 的声明。\n";
            return 1;
        }
        writeConfig(config);
        return 0;
    }

} // namespace mcdk
