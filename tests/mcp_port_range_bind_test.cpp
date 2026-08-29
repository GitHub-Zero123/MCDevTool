// 验证 MCP 服务器的端口区间绑定：多开时依次落到区间内的不同端口，
// 单值端口保持旧行为（占用时直接失败而不是静默半启动）。
#include <mcdk/mcp_server.hpp>
#include <mcdk/port_range.hpp>
#include <mcdk/settings.hpp>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace {

    // 选一段远离默认 19133 的端口，避免和开发机上真实运行的 mcdk 抢端口。
    constexpr int TestPortBegin = 29133;
    constexpr int TestPortEnd   = 29135;

    bool expect(bool condition, const std::string& description) {
        std::cout << (condition ? "[PASS] " : "[FAIL] ") << description << '\n';
        return condition;
    }

    mcdk::McpServerConfig makeConfig(int begin, int end) {
        mcdk::McpServerConfig config;
        config.enabled     = true;
        config.serverIp    = "localhost";
        config.serverPorts = mcdk::PortRange::normalized(begin, end);
        return config;
    }

    // 监听套接字进入可见状态需要一点时间，绑定后稍作等待再让下一个实例查监听表。
    void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(150)); }

} // namespace

int main() {
    bool passed = true;

    {
        mcdk::MCPServer first(makeConfig(TestPortBegin, TestPortEnd));
        first.start();
        settle();
        passed = expect(first.getBoundPort() == TestPortBegin, "第一个实例绑定到区间起始端口") && passed;

        mcdk::MCPServer second(makeConfig(TestPortBegin, TestPortEnd));
        second.start();
        settle();
        passed = expect(second.getBoundPort() == TestPortBegin + 1, "第二个实例自动落到区间内的下一个端口") && passed;

        mcdk::MCPServer third(makeConfig(TestPortBegin, TestPortEnd));
        third.start();
        settle();
        passed = expect(third.getBoundPort() == TestPortBegin + 2, "第三个实例继续向后取端口") && passed;

        mcdk::MCPServer overflow(makeConfig(TestPortBegin, TestPortEnd));
        overflow.start();
        passed = expect(overflow.getBoundPort() == 0, "区间耗尽时启动失败并报告端口 0") && passed;

        // 单值端口在被占用时必须如实失败，而不是静默绑定成一个收不到请求的服务。
        mcdk::MCPServer singleOccupied(makeConfig(TestPortBegin, TestPortBegin));
        singleOccupied.start();
        passed = expect(singleOccupied.getBoundPort() == 0, "单值端口被占用时启动失败") && passed;

        first.stop();
        second.stop();
        third.stop();
        settle();
    }

    {
        // 旧配置形态：单值端口，端口空闲时照常启动在该端口上。
        mcdk::MCPServer single(makeConfig(TestPortBegin, TestPortBegin));
        single.start();
        settle();
        passed = expect(single.getBoundPort() == TestPortBegin, "单值端口空闲时绑定到该端口") && passed;
        single.stop();
    }

    {
        // 用户把区间写反时应当归一化，而不是拿不到任何端口。
        mcdk::MCPServer reversed(makeConfig(TestPortEnd, TestPortBegin));
        reversed.start();
        settle();
        passed = expect(reversed.getBoundPort() == TestPortBegin, "起止写反的区间仍从较小端口开始绑定") && passed;
        reversed.stop();
    }

    {
        mcdk::McpServerConfig disabled = makeConfig(TestPortBegin, TestPortEnd);
        disabled.enabled               = false;
        mcdk::MCPServer server(disabled);
        server.start();
        passed = expect(server.getBoundPort() == 0, "未启用时不占用任何端口") && passed;
    }

    return passed ? 0 : 1;
}
