#include <mcdk/cli.hpp>

#include <mcdk/application.hpp>
#include <mcdk/config.hpp>
#include <mcdk/mcp_http_client.hpp>
#include <mcdk/mcp_tool_definitions.hpp>
#include <mcdk/mcp_tool_help.hpp>
#include <mcdk/plugin_declarations.hpp>
#include <mcdk/session_file.hpp>

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

#include <CLI11.hpp>
#include <mcdevtool/game_discovery.h>
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
                                  "  2  命令行用法错误\n"
                                  "  3  配置错误，或找不到可用的会话\n"
                                  "  4  启动失败（含 MCP 端口被占用、被插件否决）\n"
                                  "  5  游戏不是由 mcdk stop 结束的（自行退出、窗口被关闭或崩溃）\n"
                                  "  6  等待超时\n"
                                  "  7  工具调用返回错误";

    constexpr auto DefaultSessionName = "default";
    constexpr auto PollInterval       = std::chrono::milliseconds(200);

    struct RunOptions {
        std::string              project;
        std::string              config;
        std::string              session;
        std::vector<std::string> overrides;
        bool                     detach = false;
    };

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
            throw std::runtime_error("会话已在运行：" + name);
        }
        std::error_code error;
        fs::remove(path, error);
    }

    mcdk::UserConfig loadRunConfig(const RunOptions& options, const fs::path& configPath) {
        auto config                        = mcdk::loadUserConfig(configPath, options.overrides);
        config.mcpServer.required          = true;
        config.automation.killGameWithMcdk = true;
        config.automation.sessionName      = options.session.empty() ? DefaultSessionName : options.session;

        if (!options.session.empty()) {
            // 隔离会话：包、存档、端口都归自己，其他实例不受影响。
            config.packLinkMode        = mcdk::PackLinkMode::World;
            config.world.folderName   += "__" + options.session;
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
        try {
            config = loadRunConfig(options, configPath);
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

        auto command = quoteArgument(currentExecutablePath()) + L" run --config " + quoteArgument(configPath.wstring());
        if (!options.session.empty()) {
            command += L" --session " + quoteArgument(CLI::widen(options.session));
        }
        for (const auto& expression : options.overrides) {
            command += L" --set " + quoteArgument(CLI::widen(expression));
        }

        SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
        HANDLE              log = CreateFileW(
            logPath.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            &inherit,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &inherit, OPEN_EXISTING, 0, nullptr);
        if (log == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("无法创建会话日志：" + MCDevTool::Utils::pathToGenericUtf8(logPath));
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

    int runCommand(const RunOptions& options) {
        fs::path configPath;
        try {
            if (!options.session.empty() && !mcdk::isValidSessionName(options.session)) {
                throw std::runtime_error("会话名只能包含字母、数字、_ 和 -：" + options.session);
            }
            // --config 的相对路径相对调用者所在目录，要赶在切到 --project 之前解析。
            if (!options.config.empty()) {
                configPath = fs::absolute(fs::u8path(options.config));
            }
            enterProject(options.project);
            if (configPath.empty()) {
                configPath = fs::current_path() / ".mcdev.json";
            }
        } catch (const std::exception& exception) {
            std::cerr << "[ERROR] " << exception.what() << '\n';
            return ExitConfig;
        }

        try {
            claimSessionName(options.session.empty() ? DefaultSessionName : options.session);
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

    int waitCommand(const SessionOptions& options, const std::string& target) {
        enterProject(options.project);
        const auto wanted   = target == "exited" ? std::string(mcdk::SessionStateExited) : std::string("game_ready");
        const auto deadline = Clock::now() + std::chrono::seconds(options.timeoutSeconds);
        std::string lastError = "会话尚未出现";
        while (true) {
            try {
                const auto record = resolveSession(options.session);
                const auto state  = mcdk::effectiveSessionState(record);
                if (state == wanted) {
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
            throw std::runtime_error("工具参数必须是 JSON 对象");
        }
        return arguments;
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
        const auto record = resolveSession(options.session);
        if (!mcdk::isSessionAlive(record)) {
            throw std::runtime_error("会话已结束：" + record.name);
        }

        mcdk::McpHttpClient client({
            .host                  = record.mcpHost,
            .port                  = record.mcpPort,
            .clientName            = "mcdk-cli",
            .clientVersion         = "1",
            .requestTimeoutSeconds = options.timeoutSeconds,
        });
        std::string error;
        if (!client.initialize(error)) {
            throw std::runtime_error("无法连接会话 " + record.name + " 的 MCP：" + error);
        }

        // 服务端异步处理 initialized 通知，紧跟的请求可能先到，稍等重发。
        Json response;
        for (int attempt = 0; attempt < 40; ++attempt) {
            if (!client.post(
                    Json{
                        {"jsonrpc", "2.0"},
                        {"id", client.nextId()},
                        {"method", "tools/call"},
                        {"params", {{"name", tool}, {"arguments", arguments}}}
                    },
                    response,
                    error
                )) {
                std::cerr << "[ERROR] " << error << '\n';
                return ExitToolError;
            }
            if (!response.contains("error") || response["error"].value("code", 0) != -32600) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        const auto hasResult = response.contains("result");
        const auto result    = hasResult ? response["result"] : response;
        if (output.empty()) {
            printJson(result);
        } else {
            std::ofstream(fs::u8path(output), std::ios::binary)
                << result.dump(2, ' ', false, Json::error_handler_t::replace);
        }
        return hasResult && !result.value("isError", false) ? ExitOk : ExitToolError;
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
        return "工具（mcdk call <工具>，mcdk help <工具> 查看详情）：\n"
             + mcdk::mcp_tool_help::formatToolList(availableTools());
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
            std::cerr << "未知命令或工具：" << topic << '\n';
            return ExitUsage;
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
        run->footer(ExitCodeHelp);

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
        wait->footer(ExitCodeHelp);

        SessionOptions stopOptions{.timeoutSeconds = 30};
        bool           stopAll = false;
        auto*          stop    = app.add_subcommand("stop", "结束会话的游戏进程并等待 mcdk 退出");
        addProjectOption(stop, stopOptions.project);
        addSessionOption(stop, stopOptions.session);
        stop->add_flag("--all", stopAll, "结束本项目全部运行中的会话");
        stop->add_option("--timeout", stopOptions.timeoutSeconds, "超时秒数（默认 30）");
        stop->footer(ExitCodeHelp);

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

        try {
            app.parse(argc, argv);

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
                return waitCommand(waitOptions, waitState);
            }
            if (stop->parsed()) {
                return stopCommand(stopOptions, stopAll);
            }
            return callCommand(callOptions, callTool, callArguments, callArgumentsFile, callOutput);
        } catch (const CLI::ParseError& error) {
            return app.exit(error) == 0 ? ExitOk : ExitUsage;
        } catch (const std::exception& exception) {
            std::cerr << "[ERROR] " << exception.what() << '\n';
            return ExitConfig;
        }
    }

} // namespace mcdk::cli
