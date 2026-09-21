#pragma once
// .mcdev.json 里 plugins 数组的解析，以及由它出发只读磁盘就能得到的东西。
//
// 这里没有任何动态库加载。mcdk_stdio_bridge 由 MCP 客户端拉起，游戏没开时它也在跑，
// 却要能回答 tools/list——所以这套解析必须能脱离 mcdk 进程独立使用，也因此必须和
// mcdk 共用同一份：两份解析分叉的表现是 tools/list 与实际注册的工具对不上。
//
// 本头文件只依赖标准库、nlohmann 与 mcp，不得引入 mcdevtool 或 mcdk 运行期。
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <mcp_tool.h>
#include <nlohmann/json.hpp>

namespace mcdk {

    // .mcdev.json 的 plugins 数组中的一条。
    // 宿主不扫描目录；插件必须由用户显式声明，未声明插件不参与加载。
    struct PluginDeclaration {
        bool enabled = false;
        // 原样保留。相对路径由加载器按 .mcdev.json 所在目录解析，而非进程工作目录。
        std::string path;
        // 期望的插件 id。非空时与插件自报的 id 比对，不符则拒绝加载（防替换）。
        std::string id;
        // 传给该插件的配置，任意 JSON。序列化后原样透传，插件通过
        // mcdk.core 的 get_config 取回文本自行解析。
        std::string configJson = "null";
        // 覆盖默认的声明顺序，小者先加载。
        int priority = 0;
    };

} // namespace mcdk

namespace mcdk::plugin_host::detail {

    // 读 .mcdev.json。允许注释。失败返回 nullopt。
    [[nodiscard]] std::optional<nlohmann::json> readConfigJson(const std::filesystem::path& path);

    // plugins 数组。条目格式错误直接抛，不静默跳过——静默跳过会让
    // 「我明明写了插件却没生效」变成无从排查的问题。
    [[nodiscard]] std::vector<PluginDeclaration> parsePluginDeclarations(const nlohmann::json& root);

    // 相对路径以 .mcdev.json 所在目录为基准，而非进程工作目录；
    // 以 ~/ 开头展开为用户主目录。见 docs/plugin-system/06-loading.md §2.2。
    [[nodiscard]] std::filesystem::path
    resolvePluginPath(const std::string& raw, const std::filesystem::path& baseDirectory);

    // 项目里全部已启用插件在各自 plugin.json 的 mcpTools 中声明的工具。
    // 读不到、格式不对、插件没装，都只是少几个工具，不报错——调用方多半是
    // stdio bridge，它的职责是尽力列出，而不是替 mcdk 校验项目。
    [[nodiscard]] std::vector<mcp::tool> declaredMcpTools(const std::filesystem::path& projectRoot);

} // namespace mcdk::plugin_host::detail
