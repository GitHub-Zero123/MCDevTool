#include <mcp_server.h>

#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>

namespace {
    constexpr int TestPort = 19877;

    void require(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    mcp::server::configuration makeConfig() {
        mcp::server::configuration config;
        config.host = "127.0.0.1";
        config.port = TestPort;
        return config;
    }
} // namespace

int main() {
    try {
        mcp::server first(makeConfig());
        require(first.start(false), "first server binds");

        // 已被占用的端口必须当场报告，而不是让两个实例静默共用。
        {
            mcp::server second(makeConfig());
            require(!second.start(false), "second server reports the conflict");
        }

        // 被占用的端口上仍有服务：冲突不能影响第一个实例。
        httplib::Client client("127.0.0.1", TestPort);
        require(static_cast<bool>(client.Options("/mcp")), "first server still answers");

        first.stop();

        // 自动化循环会反复重启，停止后必须能立刻重新绑定。
        mcp::server third(makeConfig());
        require(third.start(false), "port is reusable right after stop");
        third.stop();
    } catch (const std::exception& exception) {
        std::cerr << "FAILED: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "mcp port conflict tests passed\n";
    return EXIT_SUCCESS;
}
