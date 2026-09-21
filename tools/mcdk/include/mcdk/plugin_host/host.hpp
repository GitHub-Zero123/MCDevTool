#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <mcdk/console.hpp>
#include <mcdk/plugin/abi/core.h>
#include <mcdk/settings.hpp>

namespace mcdk::runtime {
    class McpToolRegistry;
}

namespace mcdk::plugin_host {

    struct LoadedPlugin {
        std::string           id;
        std::string           name;
        std::string           version;
        std::filesystem::path path;
        std::uint32_t         abiMajor = 0;
        std::uint32_t         abiMinor = 0;
        // 某个阶段回调失败过。该插件不再收到后续阶段，但不影响其余插件与主流程。
        bool degraded = false;
    };
    // 插件宿主，加载 .mcdev.json 中声明的插件。
    class Host {
    public:
        explicit Host(ConsoleOutputCallback output);
        ~Host();

        Host(const Host&)            = delete;
        Host& operator=(const Host&) = delete;
        Host(Host&&)                 = delete;
        Host& operator=(Host&&)      = delete;
        // 按声明加载，路径相对 manifest 所在目录解析。
        void
        loadDeclared(const std::vector<PluginDeclaration>& declarations, const std::filesystem::path& baseDirectory);

        // 把各插件 plugin.json 里声明的 MCP 工具录入注册表。必须在注册窗口打开前
        // 调用：插件随后才能给它们 attach handler。
        void declareMcpTools(runtime::McpToolRegistry& registry);

        // 推进生命周期阶段，按加载顺序调用未降级插件的 on_stage。
        // 某插件失败则标记降级并继续处理其余插件。
        void advance(mcdk_stage stage);

        // 逆加载顺序终结全部插件。终结顺序的约束见
        // docs/plugin-system/03-abi-reference.md §5。
        void shutdown();

        [[nodiscard]] bool                      empty() const noexcept;
        [[nodiscard]] std::vector<LoadedPlugin> loaded() const;

    private:
        class Impl;
        std::unique_ptr<Impl> mImpl;
    };
    // 进程内唯一实例。
    [[nodiscard]] Host& instance();

} // namespace mcdk::plugin_host
