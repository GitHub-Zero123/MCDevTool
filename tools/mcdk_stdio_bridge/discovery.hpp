#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <mcdk/port_range.hpp>

namespace mcdk::bridge {

    // 一个被发现的 MCDK 实例。除 port 外的字段来自 mcdk_instance_info，
    // 旧版 MCDK 没有该工具时它们为空，此时仅靠 port / mcdkPid 区分实例。
    struct DiscoveredInstance {
        int           port          = 0;
        std::uint32_t mcdkPid       = 0;
        std::uint32_t minecraftPid  = 0;
        std::string   worldName;
        std::string   worldFolderName;
        std::string   projectRoot;
        std::string   startedAt;
        // 后端是否回应了 mcdk_instance_info
        bool          hasMetadata = false;

        // 给 AI 看的一行摘要
        [[nodiscard]] std::string describe() const;
    };

    // 在端口区间内挑出值得尝试握手的候选端口。
    //
    // Windows 上读取 TCP 监听表求交集，只返回确实有人在监听、且本机可经 loopback 连上的端口；
    // 若候选中存在 mcdk 可执行文件持有的端口，则只保留这些（软过滤，避免改名后被误杀）。
    // 其他平台或监听表读取失败时，退化为返回整个区间由调用方逐个探测。
    [[nodiscard]] std::vector<int> collectCandidatePorts(const PortRange& range);

} // namespace mcdk::bridge
