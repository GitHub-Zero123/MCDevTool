// mcdk.game.process.create 的端到端：加载 examples/03-launcher，让它接管一次进程创建。
// 用 cmd.exe 代替游戏：插件追加的参数、设置的环境变量、宿主给的 std 句柄
// 分别体现在退出码与管道输出上。
#include <mcdk/plugin_host/events.hpp>
#include <mcdk/plugin_host/host.hpp>
#include <mcdk/plugin_host/process_request.hpp>

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifndef MCDEV_TEST_LAUNCHER_PLUGIN
#error "MCDEV_TEST_LAUNCHER_PLUGIN must point at the built example plugin"
#endif

namespace {

    bool expect(bool condition, const char* description) {
        if (!condition) {
            std::cerr << "Failed: " << description << '\n';
        }
        return condition;
    }

    bool contains(const std::vector<std::string>& lines, std::string_view needle) {
        for (const auto& line : lines) {
            if (line.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    // 当前进程的环境块，UTF-8，含结尾的两个 \0。
    std::string currentEnvironmentUtf8() {
        auto* block = GetEnvironmentStringsW();
        auto* end   = block;
        while (*end != L'\0') {
            end += wcslen(end) + 1;
        }
        const int   wideLength = static_cast<int>(end - block) + 1;
        const int   length     = WideCharToMultiByte(CP_UTF8, 0, block, wideLength, nullptr, 0, nullptr, nullptr);
        std::string utf8(static_cast<std::size_t>(length), '\0');
        WideCharToMultiByte(CP_UTF8, 0, block, wideLength, utf8.data(), length, nullptr, nullptr);
        FreeEnvironmentStringsW(block);
        return utf8;
    }

} // namespace

int main() {
    using namespace mcdk;
    bool passed = true;

    std::vector<std::string> output;
    plugin_host::Host        host([&output](const std::string& message, ConsoleColor) { output.push_back(message); });

    const std::filesystem::path pluginPath = MCDEV_TEST_LAUNCHER_PLUGIN;
    host.loadDeclared(
        {PluginDeclaration{
            .enabled    = true,
            .path       = pluginPath.generic_string(),
            .configJson = R"({"extra_args":"%MCDK_LAUNCHER_CODE%","env_name":"MCDK_LAUNCHER_CODE","env_value":"7"})",
        }},
        pluginPath.parent_path()
    );
    passed &= expect(host.loaded().size() == 1, "launcher plugin loaded");
    host.advance(MCDK_STAGE_REGISTER);
    host.advance(MCDK_STAGE_CONFIG);
    host.advance(MCDK_STAGE_WORLD);

    SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
    HANDLE              readEnd  = nullptr;
    HANDLE              writeEnd = nullptr;
    CreatePipe(&readEnd, &writeEnd, &inheritable, 0);
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nullInput =
        CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr);

    char comspec[MAX_PATH]{};
    GetEnvironmentVariableA("ComSpec", comspec, MAX_PATH);
    const std::string exePath     = comspec;
    const std::string commandLine = "\"" + exePath + "\" /d /c echo launched&exit";
    const std::string environment = currentEnvironmentUtf8();

    std::optional<plugin_host::ProcessRequest> request;
    const auto                                 verdict = MCDK_EMIT_UNTIL(
        plugin_host::EventId::GameProcessCreate,
        [&] { return request->settled(); },
        [&] {
            request.emplace();
            mcdk_ev_game_process_create payload{};
            payload.struct_size  = static_cast<std::uint32_t>(sizeof(payload));
            payload.request      = request->handle();
            payload.exe_path     = {exePath.data(), exePath.size()};
            payload.command_line = {commandLine.data(), commandLine.size()};
            payload.environment  = {environment.data(), environment.size()};
            payload.std_input    = reinterpret_cast<std::uintptr_t>(nullInput);
            payload.std_output   = reinterpret_cast<std::uintptr_t>(writeEnd);
            payload.std_error    = reinterpret_cast<std::uintptr_t>(writeEnd);
            return payload;
        }
    );
    CloseHandle(writeEnd);
    CloseHandle(nullInput);

    passed &= expect(verdict.result == MCDK_EVENT_STOP, "the plugin ends dispatch with Stop after committing");
    passed &= expect(plugin_host::pluginIdOf(verdict.owner) == "com.example.launcher", "verdict names the plugin");
    passed &= expect(contains(output, "launcher:committed"), "the SDK launch helper reports success");

    auto committed = request ? request->take() : std::nullopt;
    passed        &= expect(committed.has_value(), "the host receives the committed process");
    if (committed) {
        passed &= expect(committed->ownerId == "com.example.launcher", "the commit records its owner");
        passed &= expect(
            plugin_host::commitProcess(verdict.owner, request->handle(), committed->pid, committed->tid)
                == MCDK_ERR_INVALID_HANDLE,
            "commit after the window closed is rejected"
        );

        // 与 launchGameExe 相同：恰好挂起一次，恢复后才开始跑。
        passed &= expect(ResumeThread(committed->thread) == 1, "the process was created suspended exactly once");
        passed &= expect(
            WaitForSingleObject(committed->process, 10000) == WAIT_OBJECT_0,
            "the resumed process runs to completion"
        );
        DWORD exitCode = 0;
        GetExitCodeProcess(committed->process, &exitCode);
        passed &= expect(exitCode == 7, "extra args and the injected variable reached the child (exit %VAR%)");
        CloseHandle(committed->thread);
        CloseHandle(committed->process);

        std::string text;
        char        buffer[256];
        DWORD       read = 0;
        while (ReadFile(readEnd, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
            text.append(buffer, read);
        }
        passed &= expect(text.find("launched") != std::string::npos, "the child wrote to the host's stdout pipe");
    }
    CloseHandle(readEnd);

    host.shutdown();
    if (!passed) {
        for (const auto& line : output) {
            std::cerr << line << '\n';
        }
        std::cerr << "plugin_launcher_test failed\n";
        return 1;
    }
    std::cout << "plugin_launcher_test passed\n";
    return 0;
}
