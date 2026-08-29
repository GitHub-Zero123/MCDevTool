#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace mcdk {

    // MCDK 内置 MCP 服务器的默认端口 保持与历史版本一致
    inline constexpr int DefaultMcpPort = 19133;
    // stdio bridge 缺省探测区间的上界 仅影响探测范围 不改变任何绑定行为
    inline constexpr int DefaultMcpPortRangeEnd = 19142;

    // 端口区间 单值端口表示为 begin == end 的退化区间 以兼容旧配置
    struct PortRange {
        static constexpr int MinPort = 1;
        static constexpr int MaxPort = 65535;
        // 区间跨度上限 避免用户写出 1024-65535 这类区间把探测拖死
        static constexpr int MaxSpan = 32;

        int begin = DefaultMcpPort;
        int end   = DefaultMcpPort;

        // 归一化构造 自动处理大小写顺序颠倒 越界钳制与跨度截断
        [[nodiscard]] static PortRange normalized(int first, int second);
        [[nodiscard]] static PortRange single(int port) { return normalized(port, port); }

        [[nodiscard]] bool isSingle() const noexcept { return begin == end; }
        [[nodiscard]] int  size() const noexcept { return end - begin + 1; }
        [[nodiscard]] bool contains(int port) const noexcept { return port >= begin && port <= end; }

        // 按升序展开区间内的全部端口
        [[nodiscard]] std::vector<int> ports() const;

        // 单值区间输出 "19133" 区间输出 "19133-19142"
        [[nodiscard]] std::string toString() const;

        [[nodiscard]] friend bool operator==(const PortRange&, const PortRange&) = default;
    };

    struct PortRangeParseResult {
        PortRange   range;
        // 解析失败时为 false 此时 range 保持调用方传入的回退值
        bool        ok = false;
        // 解析成功但发生了归一化调整时给出的提示 例如顺序颠倒或跨度被截断
        std::string warning;
        // 解析失败的原因
        std::string error;
    };

    // 解析端口文本 接受 "19133"(单值 旧格式) "19133-19142" "19133..19142" "19133:19142"
    // 解析失败时结果中的 range 为 fallback
    [[nodiscard]] PortRangeParseResult parsePortRange(std::string_view text, PortRange fallback = {});

    // 由两个独立数值构造区间 用于 server_port + server_port_end 这类分开书写的配置
    [[nodiscard]] PortRangeParseResult makePortRange(int first, int second);

} // namespace mcdk
