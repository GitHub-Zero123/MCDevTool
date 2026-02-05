// MCDK
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>


// mcdk modules
#include "modules/config.hpp"
#include "modules/console.hpp"
#include "modules/env.hpp"
#include "modules/hotreload.hpp"
#include "modules/level.hpp"
#include "modules/mod_dir_config.hpp"
#include "modules/mod_register.hpp"
#include "modules/style_processor.hpp"
#include "modules/utils.hpp"


// mcdevtool api
#include <mcdevtool/addon.h>
#include <mcdevtool/debug.h>
#include <mcdevtool/env.h>
#include <mcdevtool/level.h>
#include <mcdevtool/style.h>
#include <nlohmann/json.hpp>


// 默认使用"\n"而非std::endl输出日志，避免大量log的性能开销
#define _MCDEV_LOG_OUTPUT_ENDL "\n"

#ifdef MCDEV_LOG_FORCE_FLUSH_ENDL
// 强制使用std::endl
#undef _MCDEV_LOG_OUTPUT_ENDL
#define _MCDEV_LOG_OUTPUT_ENDL std::endl
#endif

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using mcdk::ReloadWatcherTask;
using mcdk::UserModDirConfig;
using mcdk::UserStyleProcessor;
using ConsoleColor = mcdk::ConsoleColor;

static std::mutex g_consoleMutex;

// 线程安全彩色输出
static void printColoredAtomic(const std::string &msg, ConsoleColor color) {
  std::lock_guard<std::mutex> lk(g_consoleMutex);
  HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);

  if (hConsole == INVALID_HANDLE_VALUE) {
    std::cout << msg << _MCDEV_LOG_OUTPUT_ENDL;
    return;
  }

  CONSOLE_SCREEN_BUFFER_INFO info;
  if (!GetConsoleScreenBufferInfo(hConsole, &info)) {
    std::cout << msg << _MCDEV_LOG_OUTPUT_ENDL;
    return;
  }

  WORD attr = 0;

  switch (color) {
  case ConsoleColor::Green:
    attr = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    break;
  case ConsoleColor::Red:
    attr = FOREGROUND_RED | FOREGROUND_INTENSITY;
    break;
  case ConsoleColor::Blue:
    attr = FOREGROUND_BLUE | FOREGROUND_INTENSITY;
    break;
  case ConsoleColor::Yellow:
    attr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
    break;
  case ConsoleColor::Cyan:
    attr = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
    break;
  case ConsoleColor::Magenta:
    attr = FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
    break;
  case ConsoleColor::White:
    attr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE |
           FOREGROUND_INTENSITY;
    break;
  case ConsoleColor::Black:
    attr = 0;
    break;
  case ConsoleColor::Gray:
    // 亮灰 = RGB，但不加高亮
    attr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    break;
  case ConsoleColor::DarkGray:
    // 深灰 = 只加高亮，不加 RGB
    attr = FOREGROUND_INTENSITY;
    break;
  default:
    break;
  }

  if (color != ConsoleColor::Default) {
    SetConsoleTextAttribute(hConsole, attr);
  }

  std::cout << msg << _MCDEV_LOG_OUTPUT_ENDL;

  if (color == ConsoleColor::Default) {
    return;
  }
  // 恢复原色
  SetConsoleTextAttribute(hConsole, info.wAttributes);
}

// 进程buffer行处理
static void processBufferAppend(
    std::string &lineBuf, const char *buf, size_t len, bool filterPython,
    const std::function<void(const std::string &)> &processLine) {
  lineBuf.append(buf, len);

  size_t pos = 0;
  while ((pos = lineBuf.find('\n')) != std::string::npos) {
    std::string line = lineBuf.substr(0, pos);
    lineBuf.erase(0, pos + 1);

    // 去除行尾可能存在的 '\r'
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    // 过滤：若启用只保留 [Python] 则丢弃其它
    if (filterPython && line.find("[Python] ") == std::string::npos)
      continue;

    processLine(line);
  }
}

#ifdef _WIN32

// pipe线程处理函数
static void
readPipeThread(HANDLE hPipe, bool filterPython,
               const std::function<void(const std::string &)> &processLine) {
  constexpr DWORD BUFSZ = 4096;
  std::string lineBuf;
  std::vector<char> buffer(BUFSZ);

  while (true) {
    DWORD bytesRead = 0;
    BOOL ok = ReadFile(hPipe, buffer.data(), BUFSZ, &bytesRead, NULL);
    if (!ok) {
      DWORD err = GetLastError();
      // ERROR_BROKEN_PIPE (109) 表示写端已关闭并读尽
      if (err == ERROR_BROKEN_PIPE) {
        // 处理残留并退出
        if (!lineBuf.empty()) {
          // 没有换行但还有内容，作为最后一行处理
          std::string lastLine = lineBuf;
          if (!lastLine.empty() && lastLine.back() == '\r')
            lastLine.pop_back();
          if (!(filterPython &&
                lastLine.find("[Python] ") == std::string::npos))
            processLine(lastLine);
          lineBuf.clear();
        }
        break;
      } else {
        // 其它错误直接退出
        break;
      }
    }

    if (bytesRead == 0) {
      // 管道关闭或无数据（通常与 ERROR_BROKEN_PIPE 一致）
      // 处理残留并退出
      if (!lineBuf.empty()) {
        std::string lastLine = lineBuf;
        if (!lastLine.empty() && lastLine.back() == '\r')
          lastLine.pop_back();
        if (!(filterPython && lastLine.find("[Python] ") == std::string::npos))
          processLine(lastLine);
        lineBuf.clear();
      }
      break;
    }

    // 追加并按行处理（会把完整行交给 processLine，残留留在 lineBuf）
    processBufferAppend(lineBuf, buffer.data(), bytesRead, filterPython,
                        processLine);
  }
}

// 尝试附加调试器到指定进程
static void debuggerAttachToProcess(DWORD pid, int port) {
  // 执行cmd调用mcdbg.exe附加（如果失败则输出错误信息）
  std::string cmd;
  cmd.reserve(48);

  cmd.append("mcdbg.exe --pid ");
  cmd.append(std::to_string(pid));
  cmd.append(" --port ");
  cmd.append(std::to_string(port));
  STARTUPINFOA si = {sizeof(si)};
  PROCESS_INFORMATION pi = {};
  if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    std::cerr << "警告：无法启动mcdbg.exe附加调试器，请确保其在环境变量路径中。"
              << _MCDEV_LOG_OUTPUT_ENDL;
    return;
  }
  std::cout << "调试器已启动，正在附加到进程PID：" << pid << " 端口：" << port
            << " ..." << _MCDEV_LOG_OUTPUT_ENDL;
}

// 检查用户配置是否需要启用IPC调试功能
static bool checkUserConfigEnableIPC(const nlohmann::json &userConfig) {
  // 若启用了auto_hot_reload_mods则启用IPC
  bool autoHotReload = userConfig.value("auto_hot_reload_mods", true);
  if (autoHotReload) {
    return true;
  }
  return false;
}

// 将utf8的string转换为utf16的wstring
static std::wstring convertUtf8ToUtf16(const std::string &utf8Str) {
  if (utf8Str.empty()) {
    return std::wstring();
  }
  int wideCharLen = MultiByteToWideChar(
      CP_UTF8, 0, utf8Str.data(), static_cast<int>(utf8Str.size()), nullptr, 0);
  if (wideCharLen == 0) {
    throw std::runtime_error("Failed to convert UTF-8 to UTF-16.");
  }
  std::wstring utf16Str(wideCharLen, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8Str.data(),
                      static_cast<int>(utf8Str.size()), &utf16Str[0],
                      wideCharLen);
  return utf16Str;
}

// 生成新的环境变量w字符串（继承当前环境变量并添加新变量）
static std::wstring createNewEnvironmentBlock(const std::wstring &newVar,
                                              const std::wstring &newValue) {
  // 获取当前环境变量块
  LPWCH envBlock = GetEnvironmentStringsW();
  if (envBlock == nullptr) {
    throw std::runtime_error("Failed to get current environment strings.");
  }

  std::wstring newEnvBlock;
  // 复制现有环境变量
  LPWCH current = envBlock;
  while (*current) {
    std::wstring varLine(current);
    newEnvBlock += varLine + L'\0';
    current += varLine.size() + 1;
  }

  // 添加新的环境变量
  newEnvBlock += newVar + L'=' + newValue + L'\0';

  // 结束环境变量块
  newEnvBlock += L'\0';

  FreeEnvironmentStringsW(envBlock);
  return newEnvBlock;
}

// 启动游戏可执行文件
static void
launchGameExe(const std::filesystem::path &exePath,
              std::string_view config = "",
              const nlohmann::json &userConfig = nlohmann::json::object(),
              const std::vector<UserModDirConfig> *modDirList = nullptr) {
  bool autoHotReload = userConfig.value("auto_hot_reload_mods", true);
  bool enableIPC = autoHotReload;
  void *lpEnvironment = nullptr;

  auto ipcServer = MCDevTool::Debug::createDebugServer();
  mcdk::ReloadWatcherTask reloadTask;
  mcdk::UserStyleProcessor styleProcessor(0, userConfig);
  reloadTask.setHotReloadAction([ipcServer](const nlohmann::json &targetPaths) {
    ipcServer->sendMessage(2, targetPaths.dump()); // FAST RELOAD
  });
  // reloadTask.bindServer(ipcServer);
  reloadTask.setOutputCallback(printColoredAtomic);
  styleProcessor.setOutputCallback(printColoredAtomic);

  std::wstring newEnv;
  if (enableIPC) {
    ipcServer->start();
    int port = ipcServer->getPort();
    // std::cout << "[MCDK] IPC调试服务器已启动，端口：" << port <<
    // _MCDEV_LOG_OUTPUT_ENDL;
    printColoredAtomic("[MCDK] IPC调试服务器已启动，端口：" +
                           std::to_string(port),
                       ConsoleColor::Green);
    newEnv = createNewEnvironmentBlock(L"MCDEV_DEBUG_IPC_PORT",
                                       std::to_wstring(port));
    lpEnvironment = (void *)newEnv.data();
  }

  STARTUPINFOW si = {sizeof(si)};
  PROCESS_INFORMATION pi = {};

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  sa.lpSecurityDescriptor = nullptr;

  // 创建 stdout/stderr 分开管道
  HANDLE outRead = NULL, outWrite = NULL;
  HANDLE errRead = NULL, errWrite = NULL;

  if (!CreatePipe(&outRead, &outWrite, &sa, 0)) {
    throw std::runtime_error("CreatePipe(stdout) failed");
  }

  if (!SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0)) {
    throw std::runtime_error("SetHandleInformation(stdout) failed");
  }

  if (!CreatePipe(&errRead, &errWrite, &sa, 0)) {
    throw std::runtime_error("CreatePipe(stderr) failed");
  }

  if (!SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0)) {
    throw std::runtime_error("SetHandleInformation(stderr) failed");
  }

  si.dwFlags |= STARTF_USESTDHANDLES;
  si.hStdOutput = outWrite;
  si.hStdError = errWrite;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

  auto neteaseConfig =
      userConfig.value("netease_config", nlohmann::json::object());

  // Build command
  std::string cmd = "\"" + exePath.string() + "\"";
  if (!neteaseConfig.value("chat_extension", false)) {
    cmd.append(" chatExtension=false");
  }

  // 自定义config启动参数
  if (!config.empty()) {
    cmd.append(" config=\"" + std::string(config) + "\"");
  }

  // ptvsd 调试参数（官方调试器接口）
  auto ptvsdConfig = mcdk::getPtvsdConfigFromJson(userConfig);
  std::string ptvsdArgs = mcdk::buildPtvsdLaunchArgs(ptvsdConfig);
  if (!ptvsdArgs.empty()) {
    cmd.append(" " + ptvsdArgs);
    printColoredAtomic("[MCDK] ptvsd 调试已启用: " + ptvsdConfig.ip + ":" +
                           std::to_string(ptvsdConfig.port),
                       ConsoleColor::Cyan);
  }

  if (!CreateProcessW(
          nullptr, convertUtf8ToUtf16(cmd).data(), nullptr, nullptr,
          TRUE, // 继承句柄
          (lpEnvironment != nullptr ? CREATE_UNICODE_ENVIRONMENT : 0),
          lpEnvironment, nullptr, &si, &pi)) {
    CloseHandle(outRead);
    CloseHandle(outWrite);
    CloseHandle(errRead);
    CloseHandle(errWrite);
    throw std::runtime_error("CreateProcessA failed");
  }

  DWORD pid = pi.dwProcessId;
  // 设置样式处理器PID
  styleProcessor.setPid(pid);

  // 父进程不需要写端
  CloseHandle(outWrite);
  CloseHandle(errWrite);

  // 输出处理回调
  auto processStdout = [](const std::string &line) {
    // 屏蔽 Engine 噪音行
    if (line.find(" [INFO][Engine] ") != std::string::npos) {
      return;
    }
    // 特殊标记行处理
    if (line.find("[INFO][Developer]") != std::string::npos) {
      printColoredAtomic(line, ConsoleColor::DarkGray);
      return;
    } else if (mcdk::containsIgnoreCase(line, "SUC")) {
      printColoredAtomic(line, ConsoleColor::Green);
      return;
    } else if (mcdk::containsIgnoreCase(line, "ERROR")) {
      printColoredAtomic(line, ConsoleColor::Red);
      return;
    } else if (mcdk::containsIgnoreCase(line, "WARN")) {
      printColoredAtomic(line, ConsoleColor::Yellow);
      return;
    } else if (mcdk::containsIgnoreCase(line, "DEBUG")) {
      printColoredAtomic(line, ConsoleColor::Cyan);
      return;
    }
    printColoredAtomic(line, ConsoleColor::Default);
  };

  // stderr 处理回调
  auto processStderr = [](const std::string &line) {
    static std::regex fileRe(R"(File \"([A-Za-z0-9_\.]+)\", line (\d+))");

    std::string out;
    out.reserve(line.size());

    std::sregex_iterator cur(line.begin(), line.end(), fileRe);
    std::sregex_iterator end;

    size_t lastPos = 0;

    for (; cur != end; ++cur) {
      const std::smatch &m = *cur;

      // 追加前面的普通内容
      out.append(line, lastPos, m.position() - lastPos);

      // 动态构造替换内容
      std::string dotted = m[1].str();
      std::string slashed = dotted;
      std::replace(slashed.begin(), slashed.end(), '.', '/');
      slashed += ".py";

      out += "File \"" + slashed + "\", line " + m[2].str();

      lastPos = m.position() + m.length();
    }

    // 拼接剩余部分
    out.append(line, lastPos);

    printColoredAtomic(out, ConsoleColor::Red);
  };

  // ===================== 用户配置后置处理 =====================
  // 是否过滤非Python输出
  bool filterPython = userConfig.value("include_debug_mod", true);
  // 调试器端口（0为不启用）
  int debuggerPort = mcdk::getEnvDebuggerPort();
  if (debuggerPort == 0) {
    // 解析用户配置覆盖
    auto debuggerConfig =
        userConfig.value("modpc_debugger", nlohmann::json::object());
    if (debuggerConfig.is_object()) {
      bool debuggerEnabled = debuggerConfig.value("enabled", false);
      if (debuggerEnabled) {
        debuggerPort = debuggerConfig.value("port", 5632);
      }
    }
  }

  // 启动两个线程并行读取（避免任何死锁）
  std::thread tOut(readPipeThread, outRead, filterPython,
                   std::function<void(const std::string &)>(processStdout));

  std::thread tErr(readPipeThread, errRead, filterPython,
                   std::function<void(const std::string &)>(processStderr));

  if (debuggerPort > 0) {
    // 尝试启动mcdbg调试器附加（在官方调试器之前的历史产物）
    debuggerAttachToProcess(pid, debuggerPort);
  }

  if (autoHotReload && modDirList != nullptr) {
    // 启动热更新追踪任务
    reloadTask.setProcessId(pid);
    // 输出追踪目录列表
    std::cout << "[HotReload] 追踪目录列表：\n";
    for (const auto &modDirConfig : *modDirList) {
      if (modDirConfig.hotReload) {
        std::cout << "  -> " << modDirConfig.getAbsoluteU8String() << "\n";
      }
    }
    reloadTask.setModDirs(UserModDirConfig::toPathList(*modDirList));
    reloadTask.start();
  }
  styleProcessor.start();

  // 等待子进程退出（子进程退出后会关闭写端，使 ReadFile 返回
  // ERROR_BROKEN_PIPE）
  WaitForSingleObject(pi.hProcess, INFINITE);

  // 停止热更新任务
  reloadTask.safeExit();
  // 停止IPC服务器 如果已启用
  ipcServer->safeExit();
  // 停止样式处理器
  styleProcessor.safeExit();

  // 等待读线程退出并关闭读端句柄
  tOut.join();
  tErr.join();

  CloseHandle(outRead);
  CloseHandle(errRead);

  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
}

#endif

// ===================== 启动游戏逻辑 =====================

// 启动游戏
static void startGame(const nlohmann::json &config) {
  auto gameExePath =
      std::filesystem::u8path(config.value("game_executable_path", ""));
  if (!std::filesystem::is_regular_file(gameExePath)) {
    // 游戏exe路径无效 重新搜索新版
    if (mcdk::updateGamePath(gameExePath)) {
      std::cout << "游戏路径无效，重新搜索：" << gameExePath.generic_string()
                << "\n";
      std::string u8input;
      std::cout << "是否更新配置文件中的游戏路径？(y/n)：";
      std::getline(std::cin, u8input);
      if (u8input == "Y" || u8input == "y") {
        mcdk::tryUpdateUserGamePath(gameExePath);
        std::cout << "已更新配置文件中的游戏路径。\n";
      } else {
        throw std::runtime_error("未更新配置文件中的游戏路径，启动终止。");
      }
    } else {
      throw std::runtime_error("未能找到有效的游戏exe文件。");
    }
  }

  if (mcdk::getEnvIsSubprocessMode()) {
    // 子进程模式 直接启动游戏exe（通常由vsc插件多开使用）
    launchGameExe(gameExePath, "", config, nullptr);
    return;
  }

  MCDevTool::cleanRuntimePacks();
  std::vector<MCDevTool::Addon::PackInfo> linkedPacks;

  auto modDirConfigs = UserModDirConfig::parseListFromJson(
      config.value("included_mod_dirs", nlohmann::json::array({"./"})));

  if (config.value("include_debug_mod", true)) {
    auto debugMod = mcdk::registerDebugMod(config, modDirConfigs);
    std::cout << "[MCDK] 已注册调试MOD：" << debugMod.uuid << "\n";
    linkedPacks.push_back(std::move(debugMod));
    mcdk::linkUserConfigModDirs(modDirConfigs, linkedPacks);
  } else {
    mcdk::linkUserConfigModDirs(modDirConfigs, linkedPacks);
  }

  // 创建世界
  auto worldFolderName = config.value("world_folder_name", "MC_DEV_WORLD");
  auto resetWorld =
      config.value("reset_world", false); // 若启用该参数 每次都会强制覆盖世界
  auto worldsPath = MCDevTool::getMinecraftWorldsPath() /
                    std::filesystem::u8path(worldFolderName);
  if (!std::filesystem::is_directory(worldsPath) || resetWorld) {
    std::filesystem::remove_all(worldsPath);
    if (resetWorld) {
      std::cout << "已删除旧世界数据，正在创建新世界...\n";
    }
    std::filesystem::create_directories(worldsPath);
    std::ofstream levelFile(worldsPath / "level.dat", std::ios::binary);
    auto levelDat = mcdk::createUserLevel(config);
    levelFile.write(reinterpret_cast<const char *>(levelDat.data()),
                    levelDat.size());
    levelFile.close();
  } else {
    // 更新level.dat的配置数据
    if (mcdk::getEnvIsPluginEnv()) {
      // 插件环境每次启动都要覆盖配置
      auto levelOptions = mcdk::parseLevelOptionsFromUserConfig(config);
      MCDevTool::Level::updateLevelDatWorldDataInFile(
          worldsPath / "level.dat", std::nullopt, levelOptions);
    } else {
      // 非插件环境只更新时间戳
      MCDevTool::Level::updateLevelDatLastPlayedInFile(worldsPath /
                                                       "level.dat");
    }
  }

  // netease_world_behavior_packs.json / netease_world_resource_packs.json
  auto behPacksManifest = nlohmann::json::array();
  auto resPacksManifest = nlohmann::json::array();
  for (const auto &pack : linkedPacks) {
    nlohmann::json packEntry{
        {"pack_id", pack.uuid},
        {"version", nlohmann::json::parse(pack.version)},
    };
    if (pack.type == MCDevTool::Addon::PackType::BEHAVIOR) {
      behPacksManifest.push_back(std::move(packEntry));
    } else if (pack.type == MCDevTool::Addon::PackType::RESOURCE) {
      resPacksManifest.push_back(std::move(packEntry));
    }
  }

  auto autoJoinGame = config.value("auto_join_game", true);
  auto envAutoJoin = mcdk::getEnvAutoJoinGameState();
  if (envAutoJoin != -1) {
    // 环境变量覆写配置文件
    autoJoinGame = (envAutoJoin == 1);
  }
  std::string targetBehJson = "netease_world_behavior_packs.json";
  std::string targetResJson = "netease_world_resource_packs.json";
  if (autoJoinGame) {
    // 使用国际版标准协议 避免网易串改
    targetBehJson = "world_behavior_packs.json";
    targetResJson = "world_resource_packs.json";
  }
  std::ofstream behManifestFile(worldsPath / targetBehJson);
  behManifestFile << behPacksManifest.dump(4);
  behManifestFile.close();

  std::ofstream resManifestFile(worldsPath / targetResJson);
  resManifestFile << resPacksManifest.dump(4);
  resManifestFile.close();

  if (!autoJoinGame) {
    // 不自动进入游戏 直接启动游戏exe
    launchGameExe(gameExePath, "", config, &modDirConfigs);
    return;
  }

  auto configPath = worldsPath / "dev_config.cppconfig";
  // 创建dev_config
  nlohmann::json devConfig{
      {"world_info", {{"level_id", worldFolderName}}},
      {"room_info", nlohmann::json::object()},
      {"player_info",
       {
           {"urs", ""},
           {"user_id", 0},
           {"user_name", config.value("user_name", "developer")},
       }},
  };

  auto defaultSkinPath =
      (gameExePath.parent_path() / "data/skin_packs/vanilla/steve.png")
          .generic_string();

  if (config.contains("skin_info") && config["skin_info"].is_object()) {
    // 用户自定义skin_info
    devConfig["skin_info"] = config["skin_info"];
    auto &skinInfo = devConfig["skin_info"];
    // 自动生成缺失字段
    if (!skinInfo.contains("slim")) {
      skinInfo["slim"] = false;
    }
    std::string skinPath = skinInfo.value("skin", "");
    if (skinPath.empty()) {
      skinInfo["skin"] = defaultSkinPath;
      skinPath = std::move(defaultSkinPath);
    }
    // 安全校验 检查文件是否存在
    if (!skinPath.empty()) {
      auto fSkinPath = std::filesystem::u8path(skinPath);
      if (!std::filesystem::is_regular_file(fSkinPath)) {
        throw std::runtime_error("自定义皮肤文件不存在：" + skinPath);
      }
    }
  } else {
    // 自动生成skin_info
    devConfig["skin_info"] = {{"slim", false},
                              {"skin", std::move(defaultSkinPath)}};
  }
  std::ofstream configFile(configPath);
  configFile << devConfig.dump(4);
  configFile.close();
  launchGameExe(gameExePath, configPath.generic_string(), config,
                &modDirConfigs);
}

#ifdef MCDK_ENABLE_CLI
#ifdef _WIN32
int MCDK_CLI_PARSE(int argc, wchar_t *argv[]);
#else
int MCDK_CLI_PARSE(int argc, char *argv[]);
#endif
#endif

#ifdef _WIN32
int wmain(int argc, wchar_t *argv[]) {
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#else
int main(int argc, char *argv[]) {
#endif

  if (mcdk::getEnvOutputMode() == 1) {
    setvbuf(stdout, nullptr, _IONBF, 0);
  }

#ifdef NDEBUG
  try {
#endif
#ifdef MCDK_ENABLE_CLI
    if (argc > 1) {
      return MCDK_CLI_PARSE(argc, argv);
    }
#endif
    auto config = mcdk::userParseConfig();
    startGame(config);
#ifdef NDEBUG
  } catch (const std::exception &e) {
    std::cerr << "[ERROR] " << e.what() << "\n";
    return 1;
  }
#endif
  return 0;
}