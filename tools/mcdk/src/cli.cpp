#include <mcdk/cli.hpp>

#ifdef _MSC_VER
// 命令行代码不在性能路径上，按体积优化。CLI11 全部内联在这个编译单元里。
#pragma optimize("s", on)
#endif

#include <mcdk/application.hpp>
#include <mcdk/config.hpp>
#include <mcdk/lane.hpp>
#include <mcdk/mcp_http_client.hpp>
#include <mcdk/mcp_tool_definitions.hpp>
#include <mcdk/mcp_tool_help.hpp>
#include <mcdk/plugin_declarations.hpp>
#include <mcdk/session_file.hpp>
#include <mcdk/session_log.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// 参数由 parse() 自己转成 UTF-8，CLI11 用不到 codecvt（它会带进上百 KB 的宽字符 locale 代码），
// 也用不到文件系统校验器。
#define CLI11_HAS_CODECVT 0
#define CLI11_HAS_FILESYSTEM 0
#include <CLI11.hpp>
#include <mcdevtool/env.h>
#include <mcdevtool/utils.h>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {
    namespace fs = std::filesystem;
    using Clock  = std::chrono::steady_clock;
    using Json   = nlohmann::ordered_json;

    enum ExitCode : int {
        ExitOk         = 0,
        ExitUsage      = 2,
        ExitConfig     = 3,
        ExitLaunch     = 4,
        ExitGameFailed = 5,
        ExitTimeout    = 6,
        ExitToolError  = 7,
    };

    constexpr auto ExitCodeHelp = "退出码：\n"
                                  "  0  成功\n"
                                  "  2  用法错误：命令、参数、工具名或工具参数名写错，stderr 会指出对应的 help\n"
                                  "  3  配置错误，或找不到可用的会话\n"
                                  "  4  启动失败（含 MCP 端口被占用、被插件否决）\n"
                                  "  5  游戏不是由 mcdk stop 结束的（自行退出、窗口被关闭或崩溃）\n"
                                  "  6  等待超时\n"
                                  "  7  工具调用返回错误";

    constexpr auto LaneHelp =
        "隔离会话（--session）使用 Lane：\n"
        "  mcdk 在游戏入口点之前注入 mcdev-lane-shim.dll，让游戏取到的 AppData 指向 Lane 目录。\n"
        "  这不是沙盒，游戏仍能访问其他路径；它只决定游戏把数据读写在哪里。\n"
        "  shim 没装上时游戏会被直接结束，不会在未隔离的状态下运行。\n"
        "  游戏的模组加载器写死了 AppData 必须是 5 段路径，所以游戏经由入口联接\n"
        "  %USERPROFILE%\\.mcdk-lanes\\<槽位> 访问 Lane 目录，数据本身仍在 Lane 目录里。\n"
        "  槽位按需复用，数量不超过同时运行的实例数，空闲槽位的联接会被自动摘掉。\n"
        "\n"
        "级别（--lane，默认 worlds）：\n"
        "  addons  只独享包目录。存档、设置、日志通过目录联接共享真实数据，存档名带 __<会话名> 后缀\n"
        "  worlds  独享包目录和存档，各会话可以使用同名存档互不冲突。设置、日志仍共享\n"
        "  full    整个数据目录独享，从空白开始，不读写你的真实设置\n"
        "\n"
        "Lane 目录（--lane-root）：\n"
        "  默认 <project>/.mcdev/lanes/<会话名>，并自动生成 .mcdev/lanes/.gitignore。\n"
        "  建议放在对应的工程或 worktree 目录下，让数据跟着代码走；不要放进系统临时目录。\n"
        "  addons、worlds 级别里有指向真实游戏数据的目录联接，删除 Lane 请用 mcdk lane remove，\n"
        "  不要手动删除，部分工具（如 Windows PowerShell 5.1 的 Remove-Item -Recurse）会顺着联接删到真实数据。\n"
        "\n";

    constexpr auto WorkflowHelp =
        "测试流程：启动一次，反复迭代\n"
        "  mcdk run --session t --detach --reuse   启动会话；已在运行且配置相同时直接复用\n"
        "  mcdk wait --session t                   阻塞到玩家进入存档\n"
        "  mcdk logs --session t                   看最新日志并拿到游标，末行 [logs] next=N\n"
        "  改代码后 mcdk call reload_scripts       同步重载改过的 py，返回重载与失败的模块\n"
        "  mcdk logs --session t --since N         只看游标之后的新日志，可加 --errors、--grep\n"
        "  不要每个小测试都重启游戏；整轮工作结束后再 mcdk stop。\n"
        "\n"
        "重载存档：\n"
        "  一次改了很多 py 时，模块逐个重载，可能读到彼此的新旧版本而报错，这不是代码本身的问题。\n"
        "  此时重载存档，不要为了让热更新通过往代码里加 getattr、try 之类的兜底。\n"
        "  先记下 mcdk ps 里的 readyCount=R，mcdk call reload_game，再 mcdk wait --after R。\n"
        "\n"
        "日志：\n"
        "  长时间运行的日志很大，不要整段读取。用 logs 的游标增量读，用 --grep/--regex/--errors 过滤；\n"
        "  要等某行出现时用 logs --wait-for <正则> --timeout <秒> 阻塞。\n"
        "\n"
        "后台模式（run --background）：\n"
        "  游戏窗口不激活、待在其他窗口之下，不碰前台和鼠标，适合自动化期间用户继续做自己的事。\n"
        "  mc_input 只能 post；视角用 turn 步骤（按角度），不能用 look。\n"
        "\n"
        "脚本化：\n"
        "  多步、重复或需要判断结果的测试，可以写 Python、PowerShell 等脚本串起 mcdk 命令。\n"
        "  每个命令都阻塞到完成或超时，用退出码判断结果；会话信息、call 的结果、logs --json 都是 JSON。\n"
        "  脚本放在工程目录里，每一步都设超时。\n"
        "\n";

    // 调用方写错了命令、参数或工具。带上应该查看的帮助主题，空表示 mcdk help。
    class UsageError : public std::runtime_error {
    public:
        UsageError(const std::string& message, std::string topic)
        : std::runtime_error(message),
          mTopic(std::move(topic)) {}

        [[nodiscard]] const std::string& topic() const noexcept { return mTopic; }

    private:
        std::string mTopic;
    };

    int reportUsage(const std::string& message, const std::string& topic) {
        std::cerr << "[ERROR] " << message << '\n'
                  << "用法见 mcdk help" << (topic.empty() ? std::string{} : " " + topic) << '\n';
        return ExitUsage;
    }

    std::size_t editDistance(std::string_view a, std::string_view b) {
        std::vector<std::size_t> row(b.size() + 1);
        for (std::size_t j = 0; j <= b.size(); ++j) {
            row[j] = j;
        }
        for (std::size_t i = 1; i <= a.size(); ++i) {
            std::size_t diagonal = row[0];
            row[0]               = i;
            for (std::size_t j = 1; j <= b.size(); ++j) {
                const auto above = row[j];
                row[j] = std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0 : 1)});
                diagonal = above;
            }
        }
        return row[b.size()];
    }

    // 与 word 最接近的候选，差得太远时不猜。
    std::string closestName(std::string_view word, const std::vector<std::string>& candidates) {
        std::string best;
        std::size_t bestDistance = std::max<std::size_t>(2, word.size() / 3) + 1;
        for (const auto& candidate : candidates) {
            const auto distance = editDistance(word, candidate);
            if (distance < bestDistance) {
                best         = candidate;
                bestDistance = distance;
            }
        }
        return best;
    }

    std::string suggestion(std::string_view word, const std::vector<std::string>& candidates) {
        const auto best = closestName(word, candidates);
        return best.empty() ? std::string{} : "，是不是 " + best + "？";
    }

    std::string joinNames(const std::vector<std::string>& names) {
        std::string text;
        for (const auto& name : names) {
            text += (text.empty() ? "" : "、") + name;
        }
        return text;
    }

    constexpr auto DefaultSessionName = "default";
    constexpr auto PollInterval       = std::chrono::milliseconds(200);

    struct RunOptions {
        std::string              project;
        std::string              config;
        std::string              session;
        std::vector<std::string> overrides;
        bool                     detach = false;
        std::string              lane;
        std::string              laneRoot;
        fs::path                 laneRootPath; // 已按调用者目录解析
        std::string              logFile;      // --detach 拉起的后台实例自己写轮转日志
        bool                     reuse      = false;
        bool                     background = false;
        std::string              fingerprint;
    };

    // 配置文件内容与影响启动结果的参数。相同才允许 --reuse 复用正在运行的会话。
    std::string runFingerprint(const RunOptions& options, const fs::path& configPath) {
        std::string material;
        if (std::ifstream input(configPath, std::ios::binary); input) {
            material.assign(std::istreambuf_iterator<char>(input), {});
        }
        for (const auto& expression : options.overrides) {
            material += '\0' + expression;
        }
        material += '\0' + (options.lane.empty() ? std::string("worlds") : options.lane);
        material += '\0' + MCDevTool::Utils::pathToGenericUtf8(options.laneRootPath);
        if (options.background) {
            material += std::string_view("\0background", 11);
        }
        std::uint64_t hash = 14695981039346656037ull;
        for (const unsigned char c : material) {
            hash = (hash ^ c) * 1099511628211ull;
        }
        return std::format("{:016x}", hash);
    }

    // 只建自己的 .gitignore，不碰上层已有的。
    void ensureIgnored(const fs::path& directory) {
        fs::create_directories(directory);
        const auto ignore = directory / ".gitignore";
        if (!fs::exists(ignore)) {
            std::ofstream(ignore, std::ios::binary) << "*\n";
        }
    }

    // Lane 目录；--lane-root 未给时用工程里的默认位置。
    fs::path laneRootFor(const RunOptions& options) {
        return options.laneRootPath.empty() ? mcdk::lane::defaultRoot(fs::current_path(), options.session)
                                            : options.laneRootPath;
    }

    struct SessionOptions {
        std::string project;
        std::string session;
        int         timeoutSeconds = 0;
    };

    void printJson(const Json& json) {
        std::cout << json.dump(2, ' ', false, Json::error_handler_t::replace) << '\n';
    }

    void enterProject(const std::string& project) {
        if (project.empty()) {
            return;
        }
        const auto path = fs::u8path(project);
        if (!fs::is_directory(path)) {
            throw std::runtime_error("项目目录不存在：" + project);
        }
        fs::current_path(path);
    }

    Json sessionToJson(const mcdk::SessionRecord& record) {
        auto json     = mcdk::sessionRecordToJson(record);
        json["state"] = mcdk::effectiveSessionState(record);
        return json;
    }

    // 未指定名称时，运行中的会话恰好只有一个才算明确。
    mcdk::SessionRecord resolveSession(const std::string& name) {
        const auto project = fs::current_path();
        if (!name.empty()) {
            auto record = mcdk::readSessionRecord(mcdk::sessionRecordPath(project, name));
            if (!record) {
                throw std::runtime_error("会话不存在：" + name);
            }
            return std::move(*record);
        }
        auto records = mcdk::listSessionRecords(project);
        std::erase_if(records, [](const auto& record) { return !mcdk::isSessionAlive(record); });
        if (records.empty()) {
            throw std::runtime_error("没有运行中的会话");
        }
        if (records.size() > 1) {
            std::string names;
            for (const auto& record : records) {
                names += (names.empty() ? "" : ", ") + record.name;
            }
            throw std::runtime_error("有多个运行中的会话，请用 --session 指定：" + names);
        }
        return std::move(records.front());
    }

    // 同名会话还在运行时拒绝，否则清掉上次的记录。
    void claimSessionName(const std::string& name) {
        const auto path = mcdk::sessionRecordPath(fs::current_path(), name);
        if (const auto record = mcdk::readSessionRecord(path); record && mcdk::isSessionAlive(*record)) {
            throw std::runtime_error("会话已在运行：" + name + "。继续使用它请加 --reuse，或先 mcdk stop");
        }
        std::error_code error;
        fs::remove(path, error);
    }

    mcdk::UserConfig loadRunConfig(const RunOptions& options, const fs::path& configPath) {
        auto config                        = mcdk::loadUserConfig(configPath, options.overrides);
        config.mcpServer.required          = true;
        config.automation.killGameWithMcdk = true;
        config.automation.sessionName      = options.session.empty() ? DefaultSessionName : options.session;
        config.automation.configFingerprint = options.fingerprint;
        if (options.background) {
            config.automation.background        = true;
            config.mcpServer.preferPostInput    = true;
            config.mcpServer.requirePostInput   = true;
            config.windowStyle.alwaysOnTop      = false;
        }

        if (!options.session.empty()) {
            // 隔离会话：数据目录按 Lane 级别隔离，端口和输入也归自己。
            const auto level = mcdk::lane::parseLevel(options.lane.empty() ? "worlds" : options.lane);
            if (!level) {
                throw std::runtime_error("--lane 只能是 addons、worlds 或 full：" + options.lane);
            }
            config.automation.laneLevel = *level;
            config.automation.laneRoot  = laneRootFor(options);
            // 包只链接到 Lane 自己的包目录，存档里不放任何联接，不受配置影响。
            config.packLinkMode = mcdk::PackLinkMode::Global;
            // addons 级别的存档目录与其他实例共享，用会话名区分。
            if (*level == mcdk::lane::Level::Addons) {
                config.world.folderName += "__" + options.session;
            }
            config.mcpServer.enabled   = true;
            config.mcpServer.preferPostInput = true;
            const bool portSpecified   = std::ranges::any_of(options.overrides, [](std::string_view expression) {
                return expression.starts_with("mcp_server_config.server_port=");
            });
            if (!portSpecified) {
                config.mcpServer.serverPort = mcdk::allocateLocalPort(config.mcpServer.serverIp);
            }
        }

        // 提前定下游戏路径，否则 startGame 会询问用户并写回配置。
        if (!fs::is_regular_file(config.gameExecutablePath)) {
            const auto& discovered = MCDevTool::autoMatchGameExePaths();
            if (discovered.empty()) {
                throw std::runtime_error("未能找到有效的游戏exe文件。");
            }
            config.gameExecutablePath = discovered.front();
            std::cout << "[MCDK] 配置中的游戏路径无效，本次使用最新版本："
                      << MCDevTool::Utils::pathToGenericUtf8(config.gameExecutablePath) << '\n';
        }
        return config;
    }

    int runForeground(const RunOptions& options, const fs::path& configPath) {
        // 管道下 stdout 默认全缓冲，调用方会读不到实时日志。
        std::setvbuf(stdout, nullptr, _IONBF, 0);

        mcdk::UserConfig config;
        // 入口槽位要占到游戏退出为止。
        std::optional<mcdk::lane::EntrySlot> laneEntry;
        try {
            config = loadRunConfig(options, configPath);
            if (config.automation.laneLevel) {
                const auto& laneRoot = config.automation.laneRoot;
                ensureIgnored(options.laneRootPath.empty() ? laneRoot.parent_path() : laneRoot);
                mcdk::lane::prepare(MCDevTool::getAppDataPath(), laneRoot, *config.automation.laneLevel);
                laneEntry.emplace(mcdk::lane::defaultEntryDirectory(), laneRoot);
                config.automation.laneEntry = laneEntry->path();
                // 之后 mcdk 计算的包目录、存档目录也走同一个入口，和游戏看到的路径一致。
                MCDevTool::setAppDataPathOverride(config.automation.laneEntry);
            }
        } catch (const std::exception& exception) {
            std::cerr << "[ERROR] " << exception.what() << '\n';
            return ExitConfig;
        }

        try {
            const auto result = mcdk::runGame(config);
            if (!result.started) {
                std::cerr << "[ERROR] 游戏启动被插件否决\n";
                return ExitLaunch;
            }
            if (result.exitCode != 0 && result.exitCode != mcdk::SessionStopExitCode) {
                std::cerr << std::format("[ERROR] 游戏异常退出，退出码 {:#010x}\n", result.exitCode);
                return ExitGameFailed;
            }
            return ExitOk;
        } catch (const std::exception& exception) {
            std::cerr << "[ERROR] " << exception.what() << '\n';
            return ExitLaunch;
        }
    }

#ifdef _WIN32
    // 按 CommandLineToArgvW 的规则转义。
    std::wstring quoteArgument(const std::wstring& argument) {
        std::wstring quoted = L"\"";
        std::size_t  slashes = 0;
        for (const wchar_t character : argument) {
            if (character == L'\\') {
                ++slashes;
                continue;
            }
            quoted.append(character == L'"' ? slashes * 2 + 1 : slashes, L'\\');
            slashes = 0;
            quoted.push_back(character);
        }
        quoted.append(slashes * 2, L'\\');
        quoted.push_back(L'"');
        return quoted;
    }

    std::wstring currentExecutablePath() {
        std::wstring path(32768, L'\0');
        path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
        return path;
    }

    int runDetached(const RunOptions& options, const fs::path& configPath) {
        const auto name    = options.session.empty() ? std::string(DefaultSessionName) : options.session;
        const auto project = fs::current_path();
        const auto logPath = mcdk::sessionsDirectory(project) / (name + ".log");
        fs::create_directories(logPath.parent_path());

        auto command = quoteArgument(currentExecutablePath()) + L" run --config " + quoteArgument(configPath.wstring())
                     + L" --log-file " + quoteArgument(logPath.wstring());
        if (!options.session.empty()) {
            command += L" --session " + quoteArgument(fs::u8path(options.session).wstring());
        }
        for (const auto& expression : options.overrides) {
            command += L" --set " + quoteArgument(fs::u8path(expression).wstring());
        }
        if (!options.lane.empty()) {
            command += L" --lane " + quoteArgument(fs::u8path(options.lane).wstring());
        }
        if (!options.laneRootPath.empty()) {
            command += L" --lane-root " + quoteArgument(options.laneRootPath.wstring());
        }
        if (options.background) {
            command += L" --background";
        }

        // 后台实例自己把输出写进轮转日志，这里只给它 NUL。
        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        HANDLE log = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
        HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &inherit, OPEN_EXISTING, 0, nullptr);
        if (log == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("无法打开 NUL 设备");
        }

        // 只继承这两个句柄。调用方的 stdout 管道一旦被后台实例持有，调用方会一直等到实例退出。
        HANDLE inherited[] = {log, input};
        SIZE_T attributeSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
        std::vector<char> attributeBuffer(attributeSize);
        auto*             attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.data());
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize)
            || !UpdateProcThreadAttribute(
                attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr
            )) {
            throw std::runtime_error("无法配置后台 mcdk 的句柄继承");
        }

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb         = sizeof(startup);
        startup.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput  = input;
        startup.StartupInfo.hStdOutput = log;
        startup.StartupInfo.hStdError  = log;
        startup.lpAttributeList        = attributes;

        // 调用方所在的作业结束时，后台实例不应被一起杀掉。不允许脱离时退回普通方式。
        PROCESS_INFORMATION process{};
        for (const DWORD flags : {DWORD{CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB}, DWORD{CREATE_NO_WINDOW}}) {
            auto mutableCommand = command;
            if (CreateProcessW(
                    nullptr,
                    mutableCommand.data(),
                    nullptr,
                    nullptr,
                    TRUE,
                    flags | EXTENDED_STARTUPINFO_PRESENT,
                    nullptr,
                    nullptr,
                    &startup.StartupInfo,
                    &process
                )) {
                break;
            }
        }
        DeleteProcThreadAttributeList(attributes);
        CloseHandle(log);
        CloseHandle(input);
        if (process.hProcess == nullptr) {
            throw std::runtime_error("无法启动后台 mcdk");
        }
        CloseHandle(process.hThread);

        const auto recordPath = mcdk::sessionRecordPath(project, name);
        const auto deadline   = Clock::now() + std::chrono::seconds(60);
        int        exitCode   = ExitTimeout;
        while (Clock::now() < deadline) {
            if (const auto record = mcdk::readSessionRecord(recordPath);
                record && record->mcdkPid == process.dwProcessId) {
                printJson(sessionToJson(*record));
                exitCode = ExitOk;
                break;
            }
            if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) {
                DWORD childExit = ExitLaunch;
                GetExitCodeProcess(process.hProcess, &childExit);
                exitCode = static_cast<int>(childExit);
                break;
            }
            std::this_thread::sleep_for(PollInterval);
        }
        CloseHandle(process.hProcess);
        if (exitCode != ExitOk) {
            std::cerr << "[ERROR] 后台启动失败，日志见 " << MCDevTool::Utils::pathToGenericUtf8(logPath) << '\n';
        }
        return exitCode;
    }

    void terminateGame(const mcdk::SessionRecord& record) {
        HANDLE game = OpenProcess(PROCESS_TERMINATE, FALSE, record.gamePid);
        if (game != nullptr) {
            TerminateProcess(game, mcdk::SessionStopExitCode);
            CloseHandle(game);
        }
    }
#else
    int runDetached(const RunOptions&, const fs::path&) { throw std::runtime_error("当前平台不支持 --detach"); }
    void terminateGame(const mcdk::SessionRecord&) { throw std::runtime_error("当前平台不支持 stop"); }
#endif

    int runCommand(RunOptions options) {
        std::optional<mcdk::RotatingOutputCapture> capture;
        if (!options.logFile.empty()) {
            capture.emplace(fs::u8path(options.logFile));
        }
        fs::path configPath;
        try {
            if (!options.session.empty() && !mcdk::isValidSessionName(options.session)) {
                throw std::runtime_error("会话名只能包含字母、数字、_ 和 -：" + options.session);
            }
            if (options.session.empty() && (!options.lane.empty() || !options.laneRoot.empty())) {
                throw std::runtime_error("--lane 和 --lane-root 需要配合 --session 使用");
            }
            // --config 等相对路径相对调用者所在目录，要赶在切到 --project 之前解析。
            if (!options.config.empty()) {
                configPath = fs::absolute(fs::u8path(options.config));
            }
            if (!options.laneRoot.empty()) {
                options.laneRootPath = fs::absolute(fs::u8path(options.laneRoot));
            }
            enterProject(options.project);
            if (configPath.empty()) {
                configPath = fs::current_path() / ".mcdev.json";
            }
        } catch (const std::exception& exception) {
            std::cerr << "[ERROR] " << exception.what() << '\n';
            return ExitConfig;
        }

        options.fingerprint = runFingerprint(options, configPath);
        const auto name     = options.session.empty() ? std::string(DefaultSessionName) : options.session;
        if (options.reuse) {
            const auto record = mcdk::readSessionRecord(mcdk::sessionRecordPath(fs::current_path(), name));
            if (record && mcdk::isSessionAlive(*record)) {
                if (record->configFingerprint != options.fingerprint) {
                    std::cerr << "[ERROR] 会话 " << name << " 正在运行，但配置或启动参数与本次不同，不能复用。"
                              << "先 mcdk stop --session " << name << " 再启动\n";
                    return ExitConfig;
                }
                printJson(sessionToJson(*record));
                return ExitOk;
            }
        }
        try {
            claimSessionName(name);
            return options.detach ? runDetached(options, configPath) : runForeground(options, configPath);
        } catch (const std::exception& exception) {
            std::cerr << "[ERROR] " << exception.what() << '\n';
            return ExitLaunch;
        }
    }

    int psCommand(const SessionOptions& options) {
        enterProject(options.project);
        auto sessions = Json::array();
        for (const auto& record : mcdk::listSessionRecords(fs::current_path())) {
            sessions.push_back(sessionToJson(record));
        }
        printJson(sessions);
        return ExitOk;
    }

    int waitCommand(const SessionOptions& options, const std::string& target, std::optional<std::uint32_t> after) {
        enterProject(options.project);
        const auto wanted   = target == "exited" ? std::string(mcdk::SessionStateExited) : std::string(mcdk::SessionStateReady);
        const auto deadline = Clock::now() + std::chrono::seconds(options.timeoutSeconds);
        std::string lastError = "会话尚未出现";
        while (true) {
            try {
                const auto record = resolveSession(options.session);
                const auto state  = mcdk::effectiveSessionState(record);
                // --after N：要等第 N 次之后的那次进入存档，用于重载存档后确认已重新进入。
                if (state == wanted && (!after || state != mcdk::SessionStateReady || record.readyCount > *after)) {
                    printJson(sessionToJson(record));
                    return ExitOk;
                }
                if (state == mcdk::SessionStateExited || state == mcdk::SessionStateLost) {
                    printJson(sessionToJson(record));
                    return ExitGameFailed;
                }
                lastError = "当前状态：" + state;
            } catch (const std::runtime_error& error) {
                // 会话可能正在启动，记录还没写出来。
                lastError = error.what();
            }
            if (Clock::now() >= deadline) {
                std::cerr << "[ERROR] 等待超时，" << lastError << '\n';
                return ExitTimeout;
            }
            std::this_thread::sleep_for(PollInterval);
        }
    }

    int stopCommand(const SessionOptions& options, bool all) {
        enterProject(options.project);
        std::vector<mcdk::SessionRecord> targets;
        if (all) {
            targets = mcdk::listSessionRecords(fs::current_path());
            std::erase_if(targets, [](const auto& record) { return !mcdk::isSessionAlive(record); });
        } else {
            targets.push_back(resolveSession(options.session));
        }

        for (const auto& record : targets) {
            if (mcdk::isSessionAlive(record)) {
                terminateGame(record);
            }
        }

        // 等到 mcdk 进程结束，此时端口已释放，同名会话可以马上重开。
        const auto deadline = Clock::now() + std::chrono::seconds(options.timeoutSeconds);
        auto       stopped  = Json::array();
        for (const auto& target : targets) {
            const auto path    = mcdk::sessionRecordPath(fs::current_path(), target.name);
            auto       current = target;
            while (mcdk::isSessionAlive(current)) {
                if (Clock::now() >= deadline) {
                    std::cerr << "[ERROR] 等待会话退出超时：" << target.name << '\n';
                    return ExitTimeout;
                }
                std::this_thread::sleep_for(PollInterval);
                current = mcdk::readSessionRecord(path).value_or(current);
            }
            stopped.push_back(sessionToJson(current));
        }
        printJson(stopped);
        return ExitOk;
    }

    int laneRemoveCommand(const SessionOptions& options, const std::string& rootOption) {
        const auto custom = rootOption.empty() ? fs::path{} : fs::absolute(fs::u8path(rootOption));
        enterProject(options.project);
        const auto record = mcdk::readSessionRecord(mcdk::sessionRecordPath(fs::current_path(), options.session));
        if (record && mcdk::isSessionAlive(*record)) {
            throw std::runtime_error("会话仍在运行，先 mcdk stop --session " + options.session);
        }
        const auto root = custom.empty() ? mcdk::lane::defaultRoot(fs::current_path(), options.session) : custom;
        if (!fs::exists(root)) {
            std::cerr << "[ERROR] Lane 不存在：" << MCDevTool::Utils::pathToGenericUtf8(root) << '\n';
            return ExitConfig;
        }
        mcdk::lane::remove(root);
        // 顺带摘掉空闲槽位的入口，其中可能有指向刚删除目录的。
        mcdk::lane::sweepEntries(mcdk::lane::defaultEntryDirectory());
        printJson(Json{{"removed", MCDevTool::Utils::pathToGenericUtf8(root)}});
        return ExitOk;
    }

    Json readCallArguments(const std::string& text, const std::string& file) {
        if (!text.empty() && !file.empty()) {
            throw CLI::ValidationError("--args 与 --args-file 只能选一个");
        }
        std::string content = text;
        if (!file.empty()) {
            std::ifstream input(fs::u8path(file), std::ios::binary);
            if (!input) {
                throw std::runtime_error("无法读取参数文件：" + file);
            }
            content.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        }
        if (content.empty()) {
            return Json::object();
        }
        auto arguments = Json::parse(content, nullptr, false);
        if (!arguments.is_object()) {
            throw UsageError("工具参数必须是 JSON 对象：" + content.substr(0, 80), "call");
        }
        return arguments;
    }

    // 一次命令内复用的 MCP 连接。析构时关闭服务端会话，否则每次调用都留下一个要闲置一小时才回收的会话。
    class SessionConnection {
    public:
        SessionConnection(const mcdk::SessionRecord& record, int timeoutSeconds)
        : mClient({
              .host                  = record.mcpHost,
              .port                  = record.mcpPort,
              .clientName            = "mcdk-cli",
              .clientVersion         = "1",
              .requestTimeoutSeconds = timeoutSeconds,
          }) {
            if (!mcdk::isSessionAlive(record)) {
                throw std::runtime_error("会话已结束：" + record.name);
            }
            std::string error;
            if (!mClient.initialize(error)) {
                throw std::runtime_error("无法连接会话 " + record.name + " 的 MCP：" + error);
            }
        }

        ~SessionConnection() { mClient.close(); }

        SessionConnection(const SessionConnection&)            = delete;
        SessionConnection& operator=(const SessionConnection&) = delete;

        // 返回 tools/call 的 result；连接或协议出错时抛出。
        Json callTool(const std::string& tool, const Json& arguments) {
            Json        response;
            std::string error;
            // 服务端异步处理 initialized 通知，紧跟的请求可能先到，稍等重发。
            for (int attempt = 0; attempt < 40; ++attempt) {
                const Json request{
                    {"jsonrpc", "2.0"},
                    {"id", mClient.nextId()},
                    {"method", "tools/call"},
                    {"params", {{"name", tool}, {"arguments", arguments}}}
                };
                if (!mClient.post(request, response, error)) {
                    throw std::runtime_error(error);
                }
                if (!response.contains("error") || response["error"].value("code", 0) != -32600) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (!response.contains("result")) {
                throw std::runtime_error(response.dump());
            }
            return response["result"];
        }

    private:
        mcdk::McpHttpClient mClient;
    };

    std::string firstText(const Json& result) {
        const auto content = result.find("content");
        if (content == result.end() || !content->is_array() || content->empty()) {
            return {};
        }
        return content->front().value("text", std::string{});
    }

    std::vector<mcp::tool> availableTools();

    // 连接游戏之前按工具定义检查，写错的工具名或参数名不会被静默忽略。
    void checkToolCall(const std::string& name, const Json& arguments) {
        const auto tools = availableTools();
        const auto tool  = std::ranges::find(tools, name, &mcp::tool::name);
        if (tool == tools.end()) {
            std::vector<std::string> names;
            for (const auto& known : tools) {
                names.push_back(known.name);
            }
            throw UsageError("未知工具：" + name + suggestion(name, names), "");
        }
        const auto& schema = tool->parameters_schema;
        if (!schema.is_object()) {
            return;
        }
        if (const auto properties = schema.find("properties");
            properties != schema.end() && properties->is_object() && !schema.value("additionalProperties", false)) {
            std::vector<std::string> known;
            for (const auto& [key, value] : properties->items()) {
                known.push_back(key);
            }
            std::vector<std::string> unknown;
            for (const auto& [key, value] : arguments.items()) {
                if (!properties->contains(key)) {
                    const auto best = closestName(key, known);
                    unknown.push_back(key + (best.empty() ? std::string{} : "（是不是 " + best + "）"));
                }
            }
            if (!unknown.empty()) {
                throw UsageError(
                    "工具 " + name + " 没有这些参数：" + joinNames(unknown)
                        + (known.empty() ? "。它不接受参数" : "。可用参数：" + joinNames(known)),
                    name
                );
            }
        }
        if (const auto required = schema.find("required"); required != schema.end() && required->is_array()) {
            std::vector<std::string> missing;
            for (const auto& key : *required) {
                if (key.is_string() && !arguments.contains(key.get<std::string>())) {
                    missing.push_back(key.get<std::string>());
                }
            }
            if (!missing.empty()) {
                throw UsageError("工具 " + name + " 缺少必需参数：" + joinNames(missing), name);
            }
        }
    }

    int callCommand(
        const SessionOptions& options,
        const std::string&    tool,
        const std::string&    argumentsText,
        const std::string&    argumentsFile,
        const std::string&    output
    ) {
        const auto arguments = readCallArguments(argumentsText, argumentsFile);
        enterProject(options.project);
        checkToolCall(tool, arguments);
        SessionConnection connection(resolveSession(options.session), options.timeoutSeconds);
        Json              result;
        try {
            result = connection.callTool(tool, arguments);
        } catch (const std::runtime_error& error) {
            std::cerr << "[ERROR] " << error.what() << '\n';
            return ExitToolError;
        }
        if (output.empty()) {
            printJson(result);
        } else {
            std::ofstream(fs::u8path(output), std::ios::binary)
                << result.dump(2, ' ', false, Json::error_handler_t::replace);
        }
        return result.value("isError", false) ? ExitToolError : ExitOk;
    }

    struct LogsOptions {
        std::optional<std::uint64_t> since;
        int                          limit = 50;
        std::string                  contains;
        std::string                  regex;
        bool                         ignoreCase = false;
        bool                         errors     = false;
        bool                         json       = false;
        std::string                  waitFor;
    };

    int logsCommand(const SessionOptions& options, const LogsOptions& logs) {
        enterProject(options.project);
        const auto        record = resolveSession(options.session);
        SessionConnection connection(record, 30);
        Json arguments{{"limit", logs.limit}, {"stream", logs.errors ? "errors" : "all"}, {"format", "json"}};
        if (!logs.contains.empty()) {
            arguments["contains"] = logs.contains;
        }
        if (!logs.regex.empty()) {
            arguments["regex"] = logs.regex;
        }
        if (logs.ignoreCase) {
            arguments["ignore_case"] = true;
        }

        auto query = [&](const Json& extra) {
            auto request = arguments;
            request.update(extra);
            const auto result = connection.callTool("read_logs", request);
            if (result.value("isError", false)) {
                throw std::runtime_error(firstText(result));
            }
            return Json::parse(firstText(result));
        };

        auto print = [&](const Json& page) {
            if (logs.json) {
                printJson(page);
                return;
            }
            for (const auto& line : page["lines"]) {
                std::cout << line.value("text", std::string{}) << '\n';
            }
            std::cout << "[logs] next=" << page.value("next", std::uint64_t{0});
            if (const auto skipped = page.value("skipped", std::uint64_t{0}); skipped > 0) {
                std::cout << " skipped=" << skipped;
            }
            if (page.value("more", false)) {
                std::cout << " more";
            }
            std::cout << '\n';
        };

        if (logs.waitFor.empty()) {
            print(query(logs.since ? Json{{"since", *logs.since}} : Json::object()));
            return ExitOk;
        }

        // 阻塞到出现匹配 --wait-for 的行。没给 --since 时从现在算起，之前的行不算。
        auto cursor = logs.since ? *logs.since : query(Json{{"limit", 1}}).value("next", std::uint64_t{0});
        const auto deadline = Clock::now() + std::chrono::seconds(options.timeoutSeconds);
        while (true) {
            const auto page = query(Json{{"since", cursor}, {"regex", logs.waitFor}});
            if (!page["lines"].empty()) {
                print(page);
                return ExitOk;
            }
            cursor = page.value("next", cursor);
            if (Clock::now() >= deadline) {
                std::cerr << "[ERROR] 等待超时，没有出现匹配 " << logs.waitFor << " 的日志\n";
                std::cout << "[logs] next=" << cursor << '\n';
                return ExitTimeout;
            }
            if (!mcdk::isSessionAlive(record)) {
                std::cerr << "[ERROR] 会话已结束\n";
                return ExitGameFailed;
            }
            std::this_thread::sleep_for(PollInterval);
        }
    }

    std::vector<mcp::tool> availableTools() {
        auto tools = mcdk::mcp_tool_definitions::buildAllTools();
        for (auto& tool : mcdk::plugin_host::detail::declaredMcpTools(fs::current_path())) {
            const auto duplicated = std::ranges::any_of(tools, [&tool](const auto& known) {
                return known.name == tool.name;
            });
            if (!duplicated) {
                tools.push_back(std::move(tool));
            }
        }
        return tools;
    }

    std::string toolListFooter() {
        return std::string(WorkflowHelp) + "工具（mcdk call <工具>，mcdk help <工具> 查看详情）：\n"
             + mcdk::mcp_tool_help::formatToolList(availableTools());
    }

    std::vector<std::string> commandNames(const CLI::App& app) {
        std::vector<std::string> names;
        for (const auto* command : app.get_subcommands([](const CLI::App*) { return true; })) {
            names.push_back(command->get_name());
        }
        return names;
    }

    // CLI11 的报错是英文，且只提示 --help；改成中文并指向出错命令自己的帮助。
    int reportParseError(const CLI::App& app, const CLI::ParseError& error) {
        std::string topic;
        for (const auto* command : app.get_subcommands()) {
            topic = command->get_name();
        }
        std::string message = error.what();
        const auto  detail  = [&message] {
            const auto colon = message.find(": ");
            return colon == std::string::npos ? message : message.substr(colon + 2);
        };
        if (dynamic_cast<const CLI::ExtrasError*>(&error) != nullptr) {
            message = "无法识别的参数：" + detail();
        } else if (dynamic_cast<const CLI::ConversionError*>(&error) != nullptr) {
            message = "参数值无法转换：" + detail();
        } else if (dynamic_cast<const CLI::RequiredError*>(&error) != nullptr) {
            const auto suffix = message.rfind(" is required");
            message           = message.starts_with("A subcommand")
                                  ? std::string("缺少子命令")
                                  : "缺少必需参数：" + (suffix == std::string::npos ? message : message.substr(0, suffix));
        } else if (dynamic_cast<const CLI::ValidationError*>(&error) != nullptr) {
            message = "参数值无效：" + message;
        }
        return reportUsage(message, topic);
    }

    int helpCommand(const CLI::App& app, const std::string& project, const std::string& topic) {
        enterProject(project);
        // App::help 会转发给已解析的子命令，也就是 help 自己，这里要绕过。
        const auto print = [](const CLI::App* target, const std::string& name) {
            std::cout << target->get_formatter()->make_help(target, name, CLI::AppFormatMode::Normal);
        };
        if (topic.empty()) {
            print(&app, "mcdk");
            return ExitOk;
        }
        if (const auto* command = app.get_subcommand_no_throw(topic)) {
            print(command, "mcdk " + topic);
            return ExitOk;
        }
        const auto tools = availableTools();
        const auto tool  = std::ranges::find(tools, topic, &mcp::tool::name);
        if (tool == tools.end()) {
            auto names = commandNames(app);
            for (const auto& known : tools) {
                names.push_back(known.name);
            }
            return reportUsage("未知命令或工具：" + topic + suggestion(topic, names), "");
        }
        std::cout << mcdk::mcp_tool_help::formatToolHelp(*tool);
        return ExitOk;
    }

    void addProjectOption(CLI::App* command, std::string& project) {
        command->add_option("--project", project, "项目目录，相对路径都以它为基准（默认当前目录）")->type_name("DIR");
    }

    void addSessionOption(CLI::App* command, std::string& session) {
        command->add_option("--session", session, "会话名（只有一个运行中的会话时可省略）")->type_name("NAME");
    }
} // namespace

namespace mcdk::cli {

#ifdef _WIN32
    int parse(int argc, wchar_t* argv[]) {
#else
    int parse(int argc, char* argv[]) {
#endif
        CLI::App app{"MCDK 游戏自动化命令行。不带参数运行 mcdk 时按 .mcdev.json 交互式启动。", "mcdk"};
        app.require_subcommand(1);
        // 工具列表和退出码自带排版，不能让 CLI11 重新折行。
        app.get_formatter()->enable_footer_formatting(false);
        app.footer(toolListFooter);

        std::string helpProject;
        std::string helpTopic;
        auto*       help = app.add_subcommand("help", "查看命令或工具的说明");
        addProjectOption(help, helpProject);
        help->add_option("topic", helpTopic, "命令名或工具名");
        help->footer("");

        RunOptions runOptions;
        auto*      run = app.add_subcommand("run", "启动游戏，全程不询问、不改配置文件");
        addProjectOption(run, runOptions.project);
        run->add_option("--config", runOptions.config, "配置文件（默认 <project>/.mcdev.json）")->type_name("FILE");
        run->add_option("--set", runOptions.overrides, "覆写配置项，可重复，如 world_name=冒烟测试")
            ->type_name("KEY=VALUE")
            ->allow_extra_args(false);
        run->add_option("--session", runOptions.session, "隔离会话名：独立的包、存档和 MCP 端口，可多开")
            ->type_name("NAME");
        run->add_flag("--detach", runOptions.detach, "后台运行，游戏进程创建后打印会话信息并返回");
        run->add_flag(
            "--reuse",
            runOptions.reuse,
            "同名会话已在运行且配置与参数相同时直接复用，打印会话信息并返回；不同则报错"
        );
        run->add_option("--lane", runOptions.lane, "Lane 级别：addons、worlds（默认）、full，需要 --session")
            ->type_name("LEVEL");
        run->add_option("--lane-root", runOptions.laneRoot, "Lane 目录，默认 <project>/.mcdev/lanes/<会话名>")
            ->type_name("DIR");
        run->add_flag(
            "--background",
            runOptions.background,
            "后台模式：游戏窗口不激活、待在其他窗口之下，游戏抢前台时交还给你；mc_input 只允许 post，"
            "转视角用 turn 步骤。适合自动化期间继续做自己的事"
        );
        run->add_option("--log-file", runOptions.logFile)->group("");
        run->footer(std::string(LaneHelp) + ExitCodeHelp);

        SessionOptions laneOptions;
        std::string    laneRemoveRoot;
        auto*          laneCommand = app.add_subcommand("lane", "管理 Lane 目录");
        laneCommand->require_subcommand(1);
        auto* laneRemove = laneCommand->add_subcommand("remove", "删除 Lane：联接只摘链接，不会删到真实游戏数据");
        addProjectOption(laneRemove, laneOptions.project);
        laneRemove->add_option("--session", laneOptions.session, "会话名")->type_name("NAME")->required();
        laneRemove->add_option("--lane-root", laneRemoveRoot, "Lane 目录，默认 <project>/.mcdev/lanes/<会话名>")
            ->type_name("DIR");

        SessionOptions psOptions;
        auto*          ps = app.add_subcommand("ps", "列出会话");
        addProjectOption(ps, psOptions.project);
        ps->footer("");

        SessionOptions waitOptions{.timeoutSeconds = 120};
        std::string    waitState = "ready";
        auto*          wait      = app.add_subcommand("wait", "等待会话进入存档或退出");
        addProjectOption(wait, waitOptions.project);
        addSessionOption(wait, waitOptions.session);
        wait->add_option("--state", waitState, "目标状态（默认 ready）")->check(CLI::IsMember({"ready", "exited"}));
        wait->add_option("--timeout", waitOptions.timeoutSeconds, "超时秒数（默认 120）");
        std::optional<std::uint32_t> waitAfter;
        wait->add_option(
                "--after",
                waitAfter,
                "等到 readyCount 大于 N 的那次进入存档。重载存档前记下会话信息里的 readyCount，"
                "传给它就不会把重载前的状态当成已就绪"
            )
            ->type_name("N");
        wait->footer(ExitCodeHelp);

        SessionOptions stopOptions{.timeoutSeconds = 30};
        bool           stopAll = false;
        auto*          stop    = app.add_subcommand("stop", "结束会话的游戏进程并等待 mcdk 退出");
        addProjectOption(stop, stopOptions.project);
        addSessionOption(stop, stopOptions.session);
        stop->add_flag("--all", stopAll, "结束本项目全部运行中的会话");
        stop->add_option("--timeout", stopOptions.timeoutSeconds, "超时秒数（默认 30）");
        stop->footer(ExitCodeHelp);

        SessionOptions logsOptions{.timeoutSeconds = 60};
        LogsOptions    logsQuery;
        auto*          logs = app.add_subcommand("logs", "按游标增量读取游戏日志，可过滤，可阻塞等待某行出现");
        addProjectOption(logs, logsOptions.project);
        addSessionOption(logs, logsOptions.session);
        logs->add_option("--since", logsQuery.since, "从上次输出末尾 [logs] next=N 的 N 开始读")->type_name("N");
        logs->add_option("--limit", logsQuery.limit, "最多输出行数（默认 50，上限 500）")
            ->check(CLI::Range(1, 500));
        logs->add_option("--grep", logsQuery.contains, "只保留包含该文本的行")->type_name("TEXT");
        logs->add_option("--regex", logsQuery.regex, "只保留匹配该正则（ECMAScript）的行")->type_name("REGEX");
        logs->add_flag("-i,--ignore-case", logsQuery.ignoreCase, "--grep/--regex/--wait-for 忽略大小写");
        logs->add_flag("--errors", logsQuery.errors, "只读 stderr；它有自己的游标");
        logs->add_flag("--json", logsQuery.json, "输出 JSON：{lines:[{seq,text}], next, skipped, more}");
        logs->add_option("--wait-for", logsQuery.waitFor, "阻塞到出现匹配该正则的行，输出匹配行后返回")
            ->type_name("REGEX");
        logs->add_option("--timeout", logsOptions.timeoutSeconds, "--wait-for 的超时秒数（默认 60）");
        logs->footer(
            "不带 --since 时输出最新的 --limit 行，用来先看一眼并拿到游标；之后带 --since 只读新增的行。\n"
            "skipped=K 表示游标之后有 K 行在读取前已被挤出内存缓冲（约保留最近 1000 行，reload_game 会清空）。\n"
            "more 表示达到 --limit，用新游标继续读。\n"
            "--wait-for 不带 --since 时只看命令开始之后的行；要避免漏掉先前触发的输出，\n"
            "先记下触发动作之前的游标再传给 --since。\n\n"
            + std::string(ExitCodeHelp)
        );

        SessionOptions callOptions{.timeoutSeconds = 60};
        std::string    callTool;
        std::string    callArguments;
        std::string    callArgumentsFile;
        std::string    callOutput;
        auto*          call = app.add_subcommand("call", "调用运行中会话的工具");
        call->add_option("tool", callTool, "工具名")->required();
        addProjectOption(call, callOptions.project);
        addSessionOption(call, callOptions.session);
        call->add_option("--args", callArguments, "工具参数，JSON 对象")->type_name("JSON");
        call->add_option("--args-file", callArgumentsFile, "从文件读取工具参数")->type_name("FILE");
        call->add_option("--output", callOutput, "结果写入文件而不是 stdout，适合截图")->type_name("FILE");
        call->add_option("--timeout", callOptions.timeoutSeconds, "超时秒数（默认 60）");

        // CLI11 的 vector 重载按逆序消费参数。
        std::vector<std::string> arguments;
        for (int index = argc - 1; index > 0; --index) {
#ifdef _WIN32
            arguments.push_back(MCDevTool::Utils::pathToUtf8(fs::path(argv[index])));
#else
            arguments.emplace_back(argv[index]);
#endif
        }

        try {
            // 拼错的命令名在 CLI11 里会被报成“缺少子命令”，先自己认出来。
            if (!arguments.empty()) {
                const auto& first = arguments.back();
                if (!first.starts_with('-') && app.get_subcommand_no_throw(first) == nullptr) {
                    return reportUsage("未知命令：" + first + suggestion(first, commandNames(app)), "");
                }
            }
            app.parse(arguments);

            if (help->parsed()) {
                return helpCommand(app, helpProject, helpTopic);
            }
            if (run->parsed()) {
                return runCommand(runOptions);
            }
            if (ps->parsed()) {
                return psCommand(psOptions);
            }
            if (wait->parsed()) {
                return waitCommand(waitOptions, waitState, waitAfter);
            }
            if (stop->parsed()) {
                return stopCommand(stopOptions, stopAll);
            }
            if (logs->parsed()) {
                return logsCommand(logsOptions, logsQuery);
            }
            if (laneRemove->parsed()) {
                return laneRemoveCommand(laneOptions, laneRemoveRoot);
            }
            return callCommand(callOptions, callTool, callArguments, callArgumentsFile, callOutput);
        } catch (const CLI::ParseError& error) {
            // --help 这类“错误”的退出码是 0，交给 CLI11 打印帮助。
            return error.get_exit_code() == 0 ? app.exit(error) : reportParseError(app, error);
        } catch (const UsageError& error) {
            return reportUsage(error.what(), error.topic());
        } catch (const std::exception& exception) {
            std::cerr << "[ERROR] " << exception.what() << '\n';
            return ExitConfig;
        }
    }

} // namespace mcdk::cli
