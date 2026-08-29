#include "discovery.hpp"

#include <algorithm>

#include <mcdk/net_listeners.hpp>
#include <mcdk/utils.hpp>

namespace mcdk::bridge {

    namespace {

        // MCP 监听者是 mcdk 自身而不是 Minecraft 进程：MCP 服务随 mcdk 一起运行，
        // 游戏进程里跑的是另一套调试 IPC。因此这里按 mcdk 可执行文件名筛选。
        constexpr std::string_view McdkImageNameHint = "mcdk";

    } // namespace

    std::string DiscoveredInstance::describe() const {
        std::string text = "port " + std::to_string(port);
        if (!worldName.empty()) {
            text += " · world \"" + worldName + "\"";
        }
        if (minecraftPid != 0) {
            text += " · minecraft pid " + std::to_string(minecraftPid);
        }
        if (mcdkPid != 0) {
            text += " · mcdk pid " + std::to_string(mcdkPid);
        }
        if (!projectRoot.empty()) {
            text += " · " + projectRoot;
        }
        if (!hasMetadata) {
            text += " · (该后端未提供实例信息，可能是较旧的 mcdk)";
        }
        return text;
    }

    std::vector<int> collectCandidatePorts(const PortRange& range) {
        if (!mcdk::net::listenerEnumerationSupported()) {
            return range.ports();
        }

        const auto listeners = mcdk::net::listListeners();
        if (listeners.empty()) {
            // 读取失败与"确实没有任何监听"无法区分，退化为全区间探测更稳妥。
            return range.ports();
        }

        std::vector<int> all;
        std::vector<int> fromMcdk;
        for (const auto& listener : listeners) {
            if (!range.contains(listener.port) || !listener.reachableFromLoopback()) {
                continue;
            }
            if (std::find(all.begin(), all.end(), listener.port) == all.end()) {
                all.push_back(listener.port);
            }
            const auto image = mcdk::net::processImageName(listener.pid);
            if (mcdk::containsIgnoreCase(image, McdkImageNameHint)
                && std::find(fromMcdk.begin(), fromMcdk.end(), listener.port) == fromMcdk.end()) {
                fromMcdk.push_back(listener.port);
            }
        }

        auto& selected = fromMcdk.empty() ? all : fromMcdk;
        std::sort(selected.begin(), selected.end());
        return selected;
    }

} // namespace mcdk::bridge
