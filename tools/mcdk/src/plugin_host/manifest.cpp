#include "manifest.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>

#include <mcdk/plugin/abi/entry.h>

namespace mcdk::plugin_host::detail {

    namespace {

        // 库选择键：<platform>.<arch>[.<config>]，由具体到通用回退。
        // 见 docs/plugin-system/06-loading.md §3.3。
        [[nodiscard]] std::string platformKey() {
#if defined(_WIN32)
            constexpr const char* platform = "windows";
#elif defined(__APPLE__)
            constexpr const char* platform = "macos";
#else
            constexpr const char* platform = "linux";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
            constexpr const char* arch = "arm64";
#else
            constexpr const char* arch = "x86_64";
#endif
            return std::string(platform) + "." + arch;
        }

        [[nodiscard]] bool isDebugBuild() noexcept {
#ifdef NDEBUG
            return false;
#else
            return true;
#endif
        }

        [[nodiscard]] mcdk_stage parseStage(const std::string& text) noexcept {
            if (text == "config") {
                return MCDK_STAGE_CONFIG;
            }
            if (text == "world") {
                return MCDK_STAGE_WORLD;
            }
            if (text == "runtime") {
                return MCDK_STAGE_RUNTIME;
            }
            if (text == "shutdown") {
                return MCDK_STAGE_SHUTDOWN;
            }
            return MCDK_STAGE_REGISTER;
        }

        [[nodiscard]] std::vector<int> splitVersion(std::string_view text) {
            std::vector<int> parts;
            std::size_t      index = 0;
            while (index < text.size()) {
                if (!std::isdigit(static_cast<unsigned char>(text[index]))) {
                    // "1.0.0-beta" 在此截断：预发布标签不参与比较。
                    break;
                }
                int value = 0;
                while (index < text.size() && std::isdigit(static_cast<unsigned char>(text[index]))) {
                    value = value * 10 + (text[index] - '0');
                    ++index;
                }
                parts.push_back(value);
                if (index < text.size() && text[index] == '.') {
                    ++index;
                } else {
                    break;
                }
            }
            return parts;
        }

        // <0 / 0 / >0
        [[nodiscard]] int compareVersion(const std::string& left, const std::string& right) {
            const auto a = splitVersion(left);
            const auto b = splitVersion(right);
            for (std::size_t index = 0; index < std::max(a.size(), b.size()); ++index) {
                const int lhs = index < a.size() ? a[index] : 0; // 缺位补 0
                const int rhs = index < b.size() ? b[index] : 0;
                if (lhs != rhs) {
                    return lhs < rhs ? -1 : 1;
                }
            }
            return 0;
        }

    } // namespace

    bool versionSatisfies(const std::string& version, const std::string& spec) {
        std::string trimmed = spec;
        trimmed.erase(0, trimmed.find_first_not_of(" \t"));
        trimmed.erase(trimmed.find_last_not_of(" \t") + 1);
        if (trimmed.empty() || trimmed == "*") {
            return true;
        }

        struct Op {
            std::string_view token;
            int              lo; // 允许的 compareVersion 结果范围
            int              hi;
        };
        // 顺序有意义：">=" 必须排在 ">" 之前，否则前缀匹配会先命中 ">"。
        static constexpr std::array<Op, 5> kOps{
            Op{">=", 0, 1},
            Op{"<=", -1, 0},
            Op{">", 1, 1},
            Op{"<", -1, -1},
            Op{"=", 0, 0},
        };
        for (const auto& op : kOps) {
            if (trimmed.rfind(op.token, 0) == 0) {
                std::string wanted = trimmed.substr(op.token.size());
                wanted.erase(0, wanted.find_first_not_of(" \t"));
                const int result = compareVersion(version, wanted);
                return result >= op.lo && result <= op.hi;
            }
        }
        // 没有运算符即精确匹配。
        return compareVersion(version, trimmed) == 0;
    }

    std::optional<PluginManifest> readManifest(const std::filesystem::path& directory, std::string& error) {
        const auto manifestPath = directory / "plugin.json";
        if (!std::filesystem::is_regular_file(manifestPath)) {
            // 刻意不回退为「在目录里猜动态库文件名」（06-loading.md §2.2 第 4 条）：
            // 猜中的那次会让用户以为清单可选，猜错的那次报错莫名其妙。
            error = "找不到 " + manifestPath.generic_string();
            return std::nullopt;
        }

        nlohmann::json root;
        try {
            std::ifstream stream(manifestPath);
            stream >> root;
        } catch (const std::exception& ex) {
            error = "plugin.json 解析失败：" + std::string(ex.what());
            return std::nullopt;
        }
        if (!root.is_object()) {
            error = "plugin.json 顶层必须是对象";
            return std::nullopt;
        }

        PluginManifest manifest;
        manifest.schema      = root.value("schema", 1);
        manifest.id          = root.value("id", std::string{});
        manifest.name        = root.value("name", std::string{});
        manifest.version     = root.value("version", std::string{});
        manifest.description = root.value("description", std::string{});
        manifest.entrySymbol = root.value("entry_symbol", std::string{MCDK_PLUGIN_ENTRY_SYMBOL});
        manifest.minStage    = parseStage(root.value("min_stage", std::string("register")));

        if (manifest.schema != 1) {
            error = "不认识的清单 schema 版本 " + std::to_string(manifest.schema);
            return std::nullopt;
        }
        if (manifest.id.empty()) {
            error = "plugin.json 缺少 id";
            return std::nullopt;
        }

        if (const auto abi = root.find("abi"); abi != root.end() && abi->is_object()) {
            manifest.abiMajor = abi->value("major", 0u);
            manifest.abiMinor = abi->value("minor", 0u);
        }

        // 库选择：先试带 config 后缀的，再退到不带的。
        const auto libraries = root.find("libraries");
        if (libraries == root.end() || !libraries->is_object()) {
            error = "plugin.json 缺少 libraries";
            return std::nullopt;
        }
        const std::string base = platformKey();
        std::string       relative;
        if (isDebugBuild()) {
            relative = libraries->value(base + ".debug", std::string{});
        }
        if (relative.empty()) {
            relative = libraries->value(base, std::string{});
        }
        if (relative.empty()) {
            error = "plugin.json 中没有当前平台（" + base + "）的产物";
            return std::nullopt;
        }
        manifest.libraryPath = std::filesystem::weakly_canonical(directory / std::filesystem::u8path(relative));

        if (const auto deps = root.find("dependencies"); deps != root.end() && deps->is_array()) {
            for (const auto& item : *deps) {
                if (!item.is_object()) {
                    continue;
                }
                PluginDependency dependency;
                dependency.id          = item.value("id", std::string{});
                dependency.versionSpec = item.value("version", std::string{});
                if (!dependency.id.empty()) {
                    manifest.dependencies.push_back(std::move(dependency));
                }
            }
        }
        if (const auto perms = root.find("permissions"); perms != root.end() && perms->is_array()) {
            for (const auto& item : *perms) {
                if (item.is_string()) {
                    manifest.permissions.push_back(item.get<std::string>());
                }
            }
        }
        return manifest;
    }

} // namespace mcdk::plugin_host::detail
