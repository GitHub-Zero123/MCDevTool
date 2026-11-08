#include <mcdk/session_file.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

#include <mcdevtool/utils.h>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

namespace mcdk {
    namespace fs = std::filesystem;

    namespace {
        using Json = nlohmann::ordered_json;

#ifdef _WIN32
        std::uint64_t processStartTime(HANDLE process) {
            FILETIME creation{};
            FILETIME exit{};
            FILETIME kernel{};
            FILETIME user{};
            if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
                return 0;
            }
            return (static_cast<std::uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
        }
#endif

        void ensureSessionsDirectory(const fs::path& directory) {
            fs::create_directories(directory);
            const auto ignore = directory / ".gitignore";
            if (!fs::exists(ignore)) {
                std::ofstream(ignore, std::ios::binary) << "*\n";
            }
        }
    } // namespace

    bool isValidSessionName(std::string_view name) noexcept {
        return !name.empty() && name.size() <= 64 && std::ranges::all_of(name, [](unsigned char character) {
            return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
                || (character >= '0' && character <= '9') || character == '_' || character == '-';
        });
    }

    fs::path sessionsDirectory(const fs::path& projectRoot) { return projectRoot / ".mcdev" / "sessions"; }

    fs::path sessionRecordPath(const fs::path& projectRoot, std::string_view name) {
        return sessionsDirectory(projectRoot) / (std::string(name) + ".json");
    }

    Json sessionRecordToJson(const SessionRecord& record) {
        Json json = {
            {"name", record.name},
            {"state", record.state},
            {"mcdkPid", record.mcdkPid},
            {"mcdkStartTime", record.mcdkStartTime},
            {"gamePid", record.gamePid},
            {"worldFolderName", record.worldFolderName},
            {"mcpHost", record.mcpHost},
            {"mcpPort", record.mcpPort},
            {"lane", record.lane},
            {"laneRoot", record.laneRoot},
            {"configFingerprint", record.configFingerprint},
            {"readyCount", record.readyCount},
        };
        if (!record.launch.empty()) {
            json["launch"] = Json::parse(record.launch, nullptr, false);
        }
        if (record.exitCode) {
            json["exitCode"] = *record.exitCode;
            json["stopped"]  = *record.exitCode == SessionStopExitCode;
        }
        return json;
    }

    void writeSessionRecord(const fs::path& path, const SessionRecord& record) {
        ensureSessionsDirectory(path.parent_path());
        auto temporary = path;
        temporary += ".tmp";
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output << sessionRecordToJson(record).dump(2);
        }
        // 读取方短暂持有文件时替换会失败，稍后重试。
        std::error_code error;
        for (int attempt = 0; attempt < 20; ++attempt) {
            fs::rename(temporary, path, error);
            if (!error) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        throw std::system_error(error, "无法写入会话记录");
    }

    std::optional<SessionRecord> readSessionRecord(const fs::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            return std::nullopt;
        }
        const auto json = Json::parse(input, nullptr, false);
        if (!json.is_object()) {
            return std::nullopt;
        }
        SessionRecord record;
        record.name            = json.value("name", "");
        record.state           = json.value("state", "");
        record.mcdkPid         = json.value("mcdkPid", std::uint32_t{0});
        record.mcdkStartTime   = json.value("mcdkStartTime", std::uint64_t{0});
        record.gamePid         = json.value("gamePid", std::uint32_t{0});
        record.worldFolderName = json.value("worldFolderName", "");
        record.mcpHost         = json.value("mcpHost", "");
        record.mcpPort         = json.value("mcpPort", 0);
        record.lane            = json.value("lane", "");
        record.laneRoot        = json.value("laneRoot", "");
        record.configFingerprint = json.value("configFingerprint", "");
        record.readyCount      = json.value("readyCount", std::uint32_t{0});
        if (const auto launch = json.find("launch"); launch != json.end()) {
            record.launch = launch->dump();
        }
        if (const auto exitCode = json.find("exitCode"); exitCode != json.end()) {
            record.exitCode = exitCode->get<std::uint32_t>();
        }
        return record;
    }

    std::vector<SessionRecord> listSessionRecords(const fs::path& projectRoot) {
        std::vector<SessionRecord> records;
        std::error_code            error;
        for (const auto& entry : fs::directory_iterator(sessionsDirectory(projectRoot), error)) {
            if (entry.path().extension() == ".json") {
                if (auto record = readSessionRecord(entry.path())) {
                    records.push_back(std::move(*record));
                }
            }
        }
        std::ranges::sort(records, {}, &SessionRecord::name);
        return records;
    }

#ifdef _WIN32
    bool isSessionAlive(const SessionRecord& record) {
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, record.mcdkPid);
        if (process == nullptr) {
            return false;
        }
        DWORD      exitCode = 0;
        const bool running  = GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE;
        const bool same     = processStartTime(process) == record.mcdkStartTime;
        CloseHandle(process);
        return running && same;
    }

    std::uint64_t currentProcessStartTime() { return processStartTime(GetCurrentProcess()); }

    int allocateLocalPort(const std::string& host) {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
        int          port   = 0;
        const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket != INVALID_SOCKET) {
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_port   = 0;
            inet_pton(AF_INET, host == "localhost" ? "127.0.0.1" : host.c_str(), &address.sin_addr);
            int length = sizeof(address);
            if (bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0
                && getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
                port = ntohs(address.sin_port);
            }
            closesocket(socket);
        }
        WSACleanup();
        if (port == 0) {
            throw std::runtime_error("无法分配 MCP 端口");
        }
        return port;
    }
#else
    bool          isSessionAlive(const SessionRecord&) { return false; }
    std::uint64_t currentProcessStartTime() { return 0; }
    int           allocateLocalPort(const std::string&) { throw std::runtime_error("当前平台不支持自动分配端口"); }
#endif

    std::string effectiveSessionState(const SessionRecord& record) {
        if (record.state != SessionStateExited && !isSessionAlive(record)) {
            return std::string(SessionStateLost);
        }
        return record.state;
    }

    SessionRecordWriter::SessionRecordWriter(fs::path path, SessionRecord record)
    : mPath(std::move(path)),
      mRecord(std::move(record)) {}

    void SessionRecordWriter::start(std::uint32_t gamePid, std::string_view state) {
        std::lock_guard lock(mMutex);
        mRecord.gamePid = gamePid;
        if (state == SessionStateReady && mRecord.state != SessionStateReady) {
            ++mRecord.readyCount;
        }
        mRecord.state = state;
        mStarted        = true;
        flush();
    }

    void SessionRecordWriter::setState(std::string_view state) {
        std::lock_guard lock(mMutex);
        if (state == SessionStateReady && mRecord.state != SessionStateReady) {
            ++mRecord.readyCount;
        }
        mRecord.state = state;
        if (mStarted) {
            flush();
        }
    }

    void SessionRecordWriter::finish(std::uint32_t exitCode) {
        std::lock_guard lock(mMutex);
        mRecord.state    = SessionStateExited;
        mRecord.exitCode = exitCode;
        flush();
    }

    void SessionRecordWriter::flush() {
        // 会话记录写失败不该影响正在运行的游戏。
        try {
            writeSessionRecord(mPath, mRecord);
        } catch (const std::exception& exception) {
            std::cerr << "[MCDK] 会话记录写入失败: " << exception.what() << '\n';
        }
    }

} // namespace mcdk
