// 端到端验证 mcdk_stdio_bridge 的实例发现与多开选择：
// 测试进程自己起两个 MCP 服务端冒充两个游戏实例，再把 bridge 拉起来当子进程驱动。
#include <mcdk/mcp_server.hpp>
#include <mcdk/port_range.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

    using json = nlohmann::json;

    // 远离默认 19133，避免撞上开发机上真实运行的 mcdk。
    constexpr int TestPortBegin = 29143;
    constexpr int TestPortEnd   = 29147;

    bool expect(bool condition, const std::string& description) {
        std::cout << (condition ? "[PASS] " : "[FAIL] ") << description << '\n';
        return condition;
    }

    std::unique_ptr<mcdk::MCPServer> startInstance(int index) {
        mcdk::McpServerConfig config;
        config.enabled     = true;
        config.serverIp    = "localhost";
        config.serverPorts = mcdk::PortRange::normalized(TestPortBegin, TestPortEnd);

        auto server = std::make_unique<mcdk::MCPServer>(config);
        mcdk::McpInstanceInfo info;
        info.mcdkPid         = static_cast<std::uint32_t>(1000 + index);
        info.projectRoot     = std::filesystem::path("D:/demo") / ("proj" + std::to_string(index));
        info.worldName       = "WORLD_" + std::to_string(index);
        info.worldFolderName = "FOLDER_" + std::to_string(index);
        info.startedAt       = "2026-01-0" + std::to_string(index + 1) + "T00:00:00.000Z";
        server->setInstanceInfo(info);
        server->setMinecraftProcessId(5000 + index);
        server->start();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return server;
    }

} // namespace

#ifdef _WIN32

namespace {

    class BridgeProcess {
    public:
        bool start(const std::string& exePath, const std::string& portRange) {
            SECURITY_ATTRIBUTES attributes{};
            attributes.nLength        = sizeof(attributes);
            attributes.bInheritHandle = TRUE;

            HANDLE childStdinRead   = nullptr;
            HANDLE childStdoutWrite = nullptr;
            if (!CreatePipe(&childStdinRead, &stdinWrite_, &attributes, 0)
                || !CreatePipe(&stdoutRead_, &childStdoutWrite, &attributes, 0)) {
                return false;
            }
            SetHandleInformation(stdinWrite_, HANDLE_FLAG_INHERIT, 0);
            SetHandleInformation(stdoutRead_, HANDLE_FLAG_INHERIT, 0);

            STARTUPINFOA startup{};
            startup.cb         = sizeof(startup);
            startup.dwFlags    = STARTF_USESTDHANDLES;
            startup.hStdInput  = childStdinRead;
            startup.hStdOutput = childStdoutWrite;
            startup.hStdError  = GetStdHandle(STD_ERROR_HANDLE);

            std::string commandLine = "\"" + exePath + "\" --port " + portRange;
            const bool  created     = CreateProcessA(
                                 nullptr,
                                 commandLine.data(),
                                 nullptr,
                                 nullptr,
                                 TRUE,
                                 CREATE_NO_WINDOW,
                                 nullptr,
                                 nullptr,
                                 &startup,
                                 &process_
                             )
                            != FALSE;

            CloseHandle(childStdinRead);
            CloseHandle(childStdoutWrite);
            return created;
        }

        ~BridgeProcess() {
            if (stdinWrite_ != nullptr) {
                CloseHandle(stdinWrite_);
            }
            if (process_.hProcess != nullptr) {
                WaitForSingleObject(process_.hProcess, 3000);
                TerminateProcess(process_.hProcess, 0);
                CloseHandle(process_.hProcess);
                CloseHandle(process_.hThread);
            }
            if (stdoutRead_ != nullptr) {
                CloseHandle(stdoutRead_);
            }
        }

        // 发一个请求，读回同一行 JSON 响应。bridge 每条响应一行。
        json request(const json& message) {
            const std::string payload = message.dump() + "\n";
            DWORD             written = 0;
            if (!WriteFile(stdinWrite_, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr)) {
                return json::object();
            }

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            std::string line;
            while (std::chrono::steady_clock::now() < deadline) {
                DWORD available = 0;
                if (!PeekNamedPipe(stdoutRead_, nullptr, 0, nullptr, &available, nullptr)) {
                    break;
                }
                if (available == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    continue;
                }

                std::string chunk(available, '\0');
                DWORD       read = 0;
                if (!ReadFile(stdoutRead_, chunk.data(), available, &read, nullptr) || read == 0) {
                    break;
                }
                line.append(chunk, 0, read);
                if (const auto newline = line.find('\n'); newline != std::string::npos) {
                    return json::parse(line.substr(0, newline), nullptr, false);
                }
            }
            return json::object();
        }

    private:
        PROCESS_INFORMATION process_{};
        HANDLE              stdinWrite_ = nullptr;
        HANDLE              stdoutRead_ = nullptr;
    };

    json callTool(BridgeProcess& bridge, int id, const std::string& name, const json& arguments = json::object()) {
        const auto response = bridge.request(json{
            {"jsonrpc", "2.0"},
            {"id", id},
            {"method", "tools/call"},
            {"params", {{"name", name}, {"arguments", arguments}}}
        });
        return response.contains("result") ? response["result"] : json::object();
    }

    std::string resultText(const json& result) {
        if (!result.contains("content") || !result["content"].is_array() || result["content"].empty()) {
            return {};
        }
        return result["content"][0].value("text", "");
    }

} // namespace

int main() {
    const std::string bridgeExe = MCDEV_TEST_BRIDGE_EXE;
    const std::string portRange = std::to_string(TestPortBegin) + "-" + std::to_string(TestPortEnd);

    auto first  = startInstance(0);
    auto second = startInstance(1);

    bool passed = expect(first->getBoundPort() == TestPortBegin, "第一个实例绑定到区间起始端口");
    passed      = expect(second->getBoundPort() == TestPortBegin + 1, "第二个实例绑定到下一个端口") && passed;
    if (!passed) {
        return 1;
    }

    BridgeProcess bridge;
    if (!expect(bridge.start(bridgeExe, portRange), "bridge 子进程启动成功")) {
        return 1;
    }

    const auto initialized = bridge.request(json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}});
    passed = expect(initialized.contains("result"), "bridge 响应 initialize") && passed;

    // 两个实例都在跑时，bridge 不应该替调用方猜目标。
    const auto ambiguous = callTool(bridge, 2, "get_latest_logs");
    passed               = expect(ambiguous.value("isError", false), "多实例且未选择时转发被拒绝") && passed;
    passed = expect(resultText(ambiguous).find("mcdk_use") != std::string::npos, "拒绝信息提示使用 mcdk_use") && passed;

    const auto instances = callTool(bridge, 3, "mcdk_instances");
    passed               = expect(!instances.value("isError", false), "mcdk_instances 调用成功") && passed;
    const auto& list =
        instances.contains("structuredContent") ? instances["structuredContent"]["instances"] : json::array();
    passed = expect(list.size() == 2, "发现两个实例") && passed;
    if (list.size() == 2) {
        passed = expect(list[0].value("port", 0) == TestPortBegin, "第一个实例端口正确") && passed;
        passed = expect(list[0].value("world_name", "") == "WORLD_0", "第一个实例世界名正确") && passed;
        passed = expect(list[1].value("world_name", "") == "WORLD_1", "第二个实例世界名正确") && passed;
        passed = expect(list[1].value("minecraft_pid", 0) == 5001, "实例携带 Minecraft pid") && passed;
    }

    const auto selected = callTool(bridge, 4, "mcdk_use", json{{"port", TestPortBegin + 1}});
    passed              = expect(!selected.value("isError", false), "mcdk_use 选中第二个实例") && passed;

    const auto info = callTool(bridge, 5, "mcdk_instance_info");
    passed          = expect(!info.value("isError", false), "选中后可以正常转发工具调用") && passed;
    passed = expect(info.contains("structuredContent") && info["structuredContent"].value("world_name", "") == "WORLD_1",
                    "转发落在被选中的实例上")
          && passed;

    // 选中的实例退出后，只剩一个实例时应当静默切换过去。
    second->stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    const auto afterExit = callTool(bridge, 6, "mcdk_instance_info");
    passed               = expect(!afterExit.value("isError", false), "选中实例退出后仍能调用") && passed;
    passed =
        expect(afterExit.contains("structuredContent")
                   && afterExit["structuredContent"].value("world_name", "") == "WORLD_0",
               "选中实例退出后自动落到仅存的实例")
        && passed;

    // 区间外的端口不接受选择。
    const auto outOfRange = callTool(bridge, 7, "mcdk_use", json{{"port", 1}});
    passed                = expect(outOfRange.value("isError", false), "区间外端口被拒绝") && passed;

    first->stop();
    return passed ? 0 : 1;
}

#else

int main() {
    std::cout << "[SKIP] bridge 发现测试目前只在 Windows 上运行\n";
    return 0;
}

#endif
