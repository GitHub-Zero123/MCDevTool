#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mcdk::net {

    // 一条 TCP 监听记录。同时覆盖 IPv4 与 IPv6：httplib 把 "localhost" 解析成 ::1 时
    // 监听项只会出现在 IPv6 表里，只查 IPv4 会漏掉本机上真实存在的服务。
    struct TcpListener {
        int           port   = 0;
        std::uint32_t pid    = 0;
        bool          isIpv6 = false;
        // 监听在环回地址（127.0.0.0/8 或 ::1）上
        bool          loopback = false;
        // 监听在任意地址（0.0.0.0 或 ::）上
        bool          anyAddress = false;

        // 本机客户端能否通过 localhost 连上这个监听
        [[nodiscard]] bool reachableFromLoopback() const noexcept { return loopback || anyAddress; }
    };

    // 当前平台是否支持枚举监听表 目前仅 Windows 支持
    [[nodiscard]] bool listenerEnumerationSupported() noexcept;

    // 枚举本机全部 TCP 监听项（IPv4 + IPv6） 不支持或读取失败时返回空
    [[nodiscard]] std::vector<TcpListener> listListeners();

    // 端口上是否已有监听者。仅在 listenerEnumerationSupported() 为真时有意义，
    // 调用方需要自行处理"不支持"的情况（此时恒为 false）。
    [[nodiscard]] bool isPortListening(int port);

    // 取进程可执行文件名（不含目录，保持原始大小写）。取不到时返回空串。
    [[nodiscard]] std::string processImageName(std::uint32_t pid);

    // 进程是否仍然存活
    [[nodiscard]] bool isProcessAlive(std::uint32_t pid);

} // namespace mcdk::net
