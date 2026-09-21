#include <mcdk/plugin_declarations.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include "manifest.hpp"

namespace mcdk::plugin_host::detail {

    std::optional<nlohmann::json> readConfigJson(const std::filesystem::path& path) {
        std::ifstream     input(path, std::ios::binary);
        const std::string content{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        // allow_exceptions=false, ignore_comments=true：配置文件是给人写的，带注释。
        auto result = nlohmann::json::parse(content, nullptr, false, true);
        if (result.is_discarded()) {
            return std::nullopt;
        }
        return result;
    }

    std::vector<PluginDeclaration> parsePluginDeclarations(const nlohmann::json& root) {
        std::vector<PluginDeclaration> declarations;
        const auto                     plugins = root.find("plugins");
        if (plugins == root.end()) {
            return declarations;
        }
        if (!plugins->is_array()) {
            throw std::runtime_error("配置文件的 plugins 字段必须是数组。");
        }

        declarations.reserve(plugins->size());
        std::size_t index = 0;
        for (const auto& item : *plugins) {
            const auto where = "plugins[" + std::to_string(index) + "]";
            ++index;
            if (!item.is_object()) {
                throw std::runtime_error(where + " 必须是对象。");
            }

            PluginDeclaration declaration;
            declaration.enabled  = item.value("enable", false);
            declaration.path     = item.value("path", "");
            declaration.id       = item.value("id", "");
            declaration.priority = item.value("priority", 0);
            if (declaration.path.empty()) {
                throw std::runtime_error(where + " 缺少 path 字段。");
            }
            // config 允许是任意 JSON 值（对象、数组、标量都行），原样序列化后
            // 透传给插件。这是「同一个插件二进制按不同参数声明多次」的基础。
            if (const auto pluginConfig = item.find("config"); pluginConfig != item.end()) {
                declaration.configJson = pluginConfig->dump();
            }
            declarations.push_back(std::move(declaration));
        }

        // 稳定排序：priority 相同者保持声明顺序，使加载顺序完全可预测。
        std::stable_sort(
            declarations.begin(),
            declarations.end(),
            [](const PluginDeclaration& left, const PluginDeclaration& right) {
                return left.priority < right.priority;
            }
        );
        return declarations;
    }

    std::filesystem::path resolvePluginPath(const std::string& raw, const std::filesystem::path& baseDirectory) {
        if (raw.size() >= 2 && raw[0] == '~' && (raw[1] == '/' || raw[1] == '\\')) {
#ifdef _WIN32
            const char* home = std::getenv("USERPROFILE");
#else
            const char* home = std::getenv("HOME");
#endif
            if (home != nullptr) {
                return std::filesystem::path(home) / std::filesystem::u8path(raw.substr(2));
            }
        }
        auto path = std::filesystem::u8path(raw);
        if (path.is_absolute()) {
            return path.lexically_normal();
        }
        return (baseDirectory / path).lexically_normal();
    }

    std::vector<mcp::tool> declaredMcpTools(const std::filesystem::path& projectRoot) {
        std::vector<mcp::tool> tools;
        const auto             config = readConfigJson(projectRoot / ".mcdev.json");
        if (!config || !config->is_object()) {
            return tools;
        }

        std::vector<PluginDeclaration> declarations;
        try {
            declarations = parsePluginDeclarations(*config);
        } catch (const std::exception&) {
            return tools;
        }

        for (const auto& declaration : declarations) {
            if (!declaration.enabled) {
                continue;
            }
            const auto path = resolvePluginPath(declaration.path, projectRoot);
            // 直指动态库的声明没有清单，声明式工具无从谈起（06-loading.md §2.2 第 5 条）。
            if (!std::filesystem::is_directory(path)) {
                continue;
            }
            std::string error;
            const auto  manifest = readManifest(path, error);
            if (!manifest) {
                continue;
            }
            if (!declaration.id.empty() && declaration.id != manifest->id) {
                continue;
            }
            for (const auto& tool : manifest->mcpTools) {
                const bool duplicate = std::any_of(
                    tools.begin(),
                    tools.end(),
                    [&tool](const mcp::tool& existing) { return existing.name == tool.name; }
                );
                // 重名时保留先声明者，与 McpToolRegistry 的规则一致。
                if (!duplicate) {
                    tools.push_back(tool);
                }
            }
        }
        return tools;
    }

} // namespace mcdk::plugin_host::detail
