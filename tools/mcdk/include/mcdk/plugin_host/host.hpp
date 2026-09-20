#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <mcdk/console.hpp>
#include <mcdk/plugin/abi/core.h>
#include <mcdk/settings.hpp>

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

    // 插件宿主。进程内只应存在一个实例。
    //
    // 加载来源只有 .mcdev.json 的 plugins 声明，宿主不扫描任何目录
    // （docs/plugin-system/06-loading.md §1）。
    class Host {
    public:
        explicit Host(ConsoleOutputCallback output);
        ~Host();

        Host(const Host&)            = delete;
        Host& operator=(const Host&) = delete;
        Host(Host&&)                 = delete;
        Host& operator=(Host&&)      = delete;

        // 按声明加载。baseDirectory 是 .mcdev.json 所在目录，相对路径以它为基准
        // 解析，而非进程工作目录。
        //
        // 单个插件的任何失败都只跳过该插件并报告，绝不终止 mcdk 启动。
        void
        loadDeclared(const std::vector<PluginDeclaration>& declarations, const std::filesystem::path& baseDirectory);

        // 推进到下一个生命周期阶段。按加载顺序依次调用每个未降级插件的 on_stage；
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

    // 进程内唯一实例，首次调用时创建并接上 printColoredAtomic。
    //
    // 用单例而非层层传参，是因为 main -> startGame -> launchGameExe 三层都要
    // 推进阶段，为此改动三处签名不划算；将来若需要多实例再改。
    [[nodiscard]] Host& instance();

} // namespace mcdk::plugin_host
