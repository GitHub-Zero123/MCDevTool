#include <mcdk/plugin_declarations.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>

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

    namespace {

        std::vector<PluginDeclaration> parseDeclarationArray(const nlohmann::json& plugins, const std::string& label) {
            if (!plugins.is_array()) {
                throw std::runtime_error(label + " 必须是数组。");
            }

            std::vector<PluginDeclaration> declarations;
            declarations.reserve(plugins.size());
            std::size_t index = 0;
            for (const auto& item : plugins) {
                const auto where = label + "[" + std::to_string(index) + "]";
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

        std::string readEnvUtf8(const char* name) {
#ifdef _WIN32
            // _wgetenv 才拿得到非 ASCII 路径；getenv 走的是系统 ANSI 代码页。
            const std::wstring wideName(name, name + std::char_traits<char>::length(name));
            const wchar_t*     value = _wgetenv(wideName.c_str());
            if (value == nullptr) {
                return {};
            }
            const auto utf8 = std::filesystem::path(value).u8string();
            return std::string(utf8.begin(), utf8.end());
#else
            const char* value = std::getenv(name);
            return value != nullptr ? std::string(value) : std::string();
#endif
        }

    } // namespace

    std::vector<PluginDeclaration> parsePluginDeclarations(const nlohmann::json& root) {
        const auto plugins = root.find("plugins");
        if (plugins == root.end()) {
            return {};
        }
        return parseDeclarationArray(*plugins, "配置文件的 plugins");
    }

    std::vector<PluginDeclaration> parseEnvPluginDeclarations() {
        const auto text = readEnvUtf8("MCDEV_PLUGINS");
        if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
            return {};
        }
        const auto plugins = nlohmann::json::parse(text, nullptr, false);
        if (plugins.is_discarded()) {
            throw std::runtime_error("环境变量 MCDEV_PLUGINS 不是合法的 JSON。");
        }
        return parseDeclarationArray(plugins, "环境变量 MCDEV_PLUGINS");
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

    std::vector<mcp::tool>
    declaredMcpTools(const std::filesystem::path& projectRoot, std::vector<std::string>* problems) {
        const auto report = [problems](std::string message) {
            if (problems != nullptr) {
                problems->push_back(std::move(message));
            }
        };

        std::vector<mcp::tool>         tools;
        std::vector<PluginDeclaration> declarations;
        const auto                     configPath = projectRoot / ".mcdev.json";
        std::error_code                ignored;
        if (std::filesystem::exists(configPath, ignored)) {
            const auto config = readConfigJson(configPath);
            if (!config || !config->is_object()) {
                report(configPath.generic_string() + " 不是合法的 JSON 对象");
            } else {
                try {
                    declarations = parsePluginDeclarations(*config);
                } catch (const std::exception& error) {
                    report(configPath.generic_string() + "：" + error.what());
                }
            }
        }
        // 编辑器用同一个变量拉起 bridge 时，它注入的插件的工具也要列出来。
        try {
            auto external = parseEnvPluginDeclarations();
            declarations.insert(
                declarations.end(),
                std::make_move_iterator(external.begin()),
                std::make_move_iterator(external.end())
            );
        } catch (const std::exception& error) {
            report(error.what());
        }

        for (const auto& declaration : declarations) {
            if (!declaration.enabled) {
                continue;
            }
            const auto path = resolvePluginPath(declaration.path, projectRoot);
            if (!std::filesystem::exists(path, ignored)) {
                report("插件路径不存在：" + path.generic_string());
                continue;
            }
            // 直指动态库的声明没有清单，声明式工具无从谈起（06-loading.md §2.2 第 5 条）。这是设计，不报。
            if (!std::filesystem::is_directory(path, ignored)) {
                continue;
            }
            std::string error;
            const auto  manifest = readManifest(path, error);
            if (!manifest) {
                report(path.generic_string() + "：" + error);
                continue;
            }
            if (!declaration.id.empty() && declaration.id != manifest->id) {
                report(
                    path.generic_string() + "：声明的 id 是 " + declaration.id + "，清单里却是 " + manifest->id
                    + "，按防替换规则跳过"
                );
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
