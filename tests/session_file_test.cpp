#include <mcdk/session_file.hpp>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {
    namespace fs = std::filesystem;

    void require(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    mcdk::SessionRecord makeRecord(const char* name) {
        return {
            .name            = name,
            .state           = "process_started",
            .mcdkPid         = GetCurrentProcessId(),
            .mcdkStartTime   = mcdk::currentProcessStartTime(),
            .gamePid         = 1234,
            .worldFolderName = "世界__a",
            .mcpHost         = "127.0.0.1",
            .mcpPort         = 19140,
            .isolated        = true,
        };
    }

    void testNames() {
        require(mcdk::isValidSessionName("smoke-1_A"), "plain name");
        for (const auto* name : {"", "a b", "a/b", "..", "a.b", "中文"}) {
            require(!mcdk::isValidSessionName(name), name);
        }
    }

    void testRoundTrip(const fs::path& project) {
        const auto path = mcdk::sessionRecordPath(project, "a");
        mcdk::writeSessionRecord(path, makeRecord("a"));
        require(fs::is_regular_file(mcdk::sessionsDirectory(project) / ".gitignore"), "sessions dir is ignored");

        const auto record = mcdk::readSessionRecord(path);
        require(record.has_value(), "record is readable");
        require(record->worldFolderName == "世界__a", "utf-8 survives");
        require(record->mcpPort == 19140 && record->isolated && !record->exitCode, "fields survive");
        require(!mcdk::readSessionRecord(project / "missing.json"), "missing record");

        std::ofstream(project / "broken.json", std::ios::binary) << "{ nope";
        require(!mcdk::readSessionRecord(project / "broken.json"), "broken record");
    }

    void testLiveness() {
        auto record = makeRecord("a");
        require(mcdk::isSessionAlive(record), "own process is alive");
        require(mcdk::effectiveSessionState(record) == "process_started", "live state is kept");

        // pid 被别的进程复用时，启动时间对不上。
        record.mcdkStartTime += 1;
        require(!mcdk::isSessionAlive(record), "reused pid is not alive");
        require(mcdk::effectiveSessionState(record) == mcdk::SessionStateLost, "dead session is lost");

        record.state = mcdk::SessionStateExited;
        require(mcdk::effectiveSessionState(record) == mcdk::SessionStateExited, "exited stays exited");
    }

    void testWriter(const fs::path& project) {
        const auto path = mcdk::sessionRecordPath(project, "writer");
        std::error_code ignored;
        fs::remove(path, ignored);

        mcdk::SessionRecordWriter writer(path, makeRecord("writer"));
        writer.setState("game_ready");
        require(!fs::exists(path), "nothing is written before start");

        writer.start(4321, "process_started");
        require(mcdk::readSessionRecord(path)->gamePid == 4321, "start writes the game pid");

        writer.setState("game_ready");
        require(mcdk::readSessionRecord(path)->state == "game_ready", "state update");

        writer.finish(mcdk::SessionStopExitCode);
        const auto record = mcdk::readSessionRecord(path);
        require(record->state == mcdk::SessionStateExited, "finish state");
        require(record->exitCode == mcdk::SessionStopExitCode, "finish exit code");
        require(mcdk::sessionRecordToJson(*record)["stopped"] == true, "stop is distinguishable");
    }

    void testList(const fs::path& project) {
        mcdk::writeSessionRecord(mcdk::sessionRecordPath(project, "b"), makeRecord("b"));
        std::ofstream(mcdk::sessionsDirectory(project) / "b.log", std::ios::binary) << "log";
        const auto records = mcdk::listSessionRecords(project);
        require(records.size() == 3, "only records are listed");
        require(records[0].name == "a" && records[1].name == "b", "records are sorted");
        require(mcdk::listSessionRecords(project / "none").empty(), "no sessions directory");
    }

    void testPortAllocation() {
        const int port = mcdk::allocateLocalPort("localhost");
        require(port > 0 && port <= 65535, "allocated port");
    }
} // namespace

int main() {
    const auto project = fs::temp_directory_path() / fs::u8path("mcdk_会话_测试");
    try {
        fs::remove_all(project);
        fs::create_directories(project);
        testNames();
        testRoundTrip(project);
        testLiveness();
        testWriter(project);
        testList(project);
        testPortAllocation();
        fs::remove_all(project);
    } catch (const std::exception& exception) {
        std::cerr << "FAILED: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "session file tests passed\n";
    return EXIT_SUCCESS;
}
