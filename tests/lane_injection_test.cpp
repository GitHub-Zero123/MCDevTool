// 端到端验证 mcdk 的 Lane 注入：挂起创建探针进程，排队加载 shim，恢复后确认它取到的是 Lane 目录。
#include <mcdk/lane.hpp>

#include <mcdev_lane_shim.h>

#include <chrono>
#include <cstring>
#include <iterator>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

namespace {
    namespace fs = std::filesystem;

    void require(bool condition, const std::string& message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    std::wstring knownFolder(REFKNOWNFOLDERID id) {
        PWSTR path = nullptr;
        SHGetKnownFolderPath(id, 0, nullptr, &path);
        std::wstring result = path != nullptr ? path : L"";
        CoTaskMemFree(path);
        return result;
    }

    struct Launch {
        DWORD                       exitCode = 0;
        std::optional<std::string> error;
        std::wstring                roaming;
        std::wstring                local;
    };

    // 环境块里只放给定的 Lane 变量，其余照抄当前进程。
    std::wstring environmentWith(const std::wstring& laneRoot) {
        std::wstring block;
        if (wchar_t* current = GetEnvironmentStringsW()) {
            for (const wchar_t* entry = current; *entry != L'\0'; entry += wcslen(entry) + 1) {
                if (std::wstring_view(entry).starts_with(MCDEV_LANE_ROOT_ENV)) continue;
                block.append(entry).push_back(L'\0');
            }
            FreeEnvironmentStringsW(current);
        }
        if (!laneRoot.empty()) {
            block.append(std::wstring(MCDEV_LANE_ROOT_ENV) + L"=" + laneRoot).push_back(L'\0');
        }
        block.push_back(L'\0');
        return block;
    }

    Launch launchProbe(const fs::path& output, const std::wstring& laneRoot) {
        std::wstring command = L"\"" + fs::path(MCDK_LANE_PROBE_PATH).wstring() + L"\" \"" + output.wstring() + L"\"";
        auto         env     = environmentWith(laneRoot);
        STARTUPINFOW        startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        require(
            CreateProcessW(
                nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                env.data(), nullptr, &startup, &process
            ) != FALSE,
            "probe launches"
        );

        Launch launch;
        try {
            mcdk::lane::ShimInjection shim(process.hProcess, process.hThread, process.dwProcessId, MCDK_LANE_SHIM_PATH);
            ResumeThread(process.hThread);
            shim.waitArmed(std::chrono::seconds(10));
        } catch (const std::exception& error) {
            launch.error = error.what();
        }
        ResumeThread(process.hThread);
        WaitForSingleObject(process.hProcess, 10000);
        GetExitCodeProcess(process.hProcess, &launch.exitCode);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);

        std::ifstream input(output, std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        std::wstring text(bytes.size() / sizeof(wchar_t), L' ');
        std::memcpy(text.data(), bytes.data(), text.size() * sizeof(wchar_t));
        const auto first = text.find(L'\n');
        if (first != std::wstring::npos) {
            launch.roaming = text.substr(0, first);
            const auto second = text.find(L'\n', first + 1);
            launch.local      = text.substr(first + 1, second == std::wstring::npos ? second : second - first - 1);
        }
        return launch;
    }

    void testRedirectsRoamingOnly(const fs::path& base) {
        const auto root   = base / L"Lane 根目录";
        fs::create_directories(root);
        const auto launch = launchProbe(base / "ok.txt", root.wstring());
        require(!launch.error.has_value(), "shim arms: " + launch.error.value_or(""));
        require(launch.exitCode == 0, "probe exits normally");
        require(launch.roaming == root.wstring(), "Roaming AppData points into the lane");
        require(launch.local == knownFolder(FOLDERID_LocalAppData), "Local AppData is untouched");
    }

    void testRefusesWithoutRoot(const fs::path& base) {
        const auto output = base / "refused.txt";
        const auto launch = launchProbe(output, L"");
        require(launch.error.has_value(), "injection reports the failure");
        require(launch.exitCode == MCDEV_LANE_EXIT_SETUP_FAILED, "shim ends the process instead of running unisolated");
        require(!fs::exists(output), "the probe never ran its own code");
    }

    void testMissingShimIsRejected() {
        bool rejected = false;
        try {
            mcdk::lane::ShimInjection shim(nullptr, nullptr, 0, fs::path(MCDK_LANE_SHIM_PATH).parent_path() / "missing.dll");
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected, "a missing shim stops the launch");
    }
} // namespace

int main() {
    const auto base = fs::temp_directory_path() / fs::u8path("mcdk_Lane 注入_测试");
    try {
        fs::remove_all(base);
        fs::create_directories(base);
        testRedirectsRoamingOnly(base);
        testRefusesWithoutRoot(base);
        testMissingShimIsRejected();
        fs::remove_all(base);
    } catch (const std::exception& exception) {
        std::cerr << "FAILED: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "lane injection tests passed\n";
    return EXIT_SUCCESS;
}
