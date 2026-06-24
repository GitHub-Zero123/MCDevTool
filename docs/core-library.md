# mcdk_core 核心模式库

mcdk_core 把原先写死在 `tools/mcdk/main.cpp` 里的启动逻辑，抽取成独立的可复用 STATIC 库。其他业务可以用几行代码 + 纯内存 config 启动完整的开发测试世界（含世界生成、Addon 注册、IPC、MCP、热更新、样式处理器），而**不必依赖磁盘上的 `.mcdev.json` 文件**。

## 设计目标

- 对外暴露纯 API（`mcdk::core::Launcher::run`），启动器逻辑与 `mcdk.exe` 解耦。
- **强类型配置 `LaunchConfig`**：字段有类型、有默认值，字段名拼错编译期即报错，不再是裸 `nlohmann::json` 数驱。
- **文件写入 / 交互可控**：通过 `ConfigFilePolicy` 与 `PromptCallback` 策略化——核心模式默认不碰磁盘 `.mcdev.json`、不弹 `cin` 交互。
- 行为与原 `mcdk.exe` **完全一致**：CLI 走 `fromJson` + `ReadWrite` + cin prompter 的兼容路径，零行为变更。

## 依赖关系

`mcdk_core` 默认全套链，CMake 提供两个 option 供高级用户按需裁剪：

| 依赖 | 说明 | 关闭 option |
|------|------|-------------|
| `mcdevtool` | 核心 API（世界/Addon/Level/Debug/Style/Env），携带 NBT | 始终启用 |
| `mcp` | MCP 协议服务器 | `-DMCDK_CORE_ENABLE_MCP=OFF` |
| `MCDEV_MOD_RESOURCE` | 内嵌调试 MOD Python 资源 | `-DMCDK_CORE_ENABLE_MOD_RESOURCE=OFF` |

> 注意：core.cpp 的 modules 头（`mcp_server.hpp`、`mod_register.hpp`）内部依赖 mcp 与 INCLUDE_MOD.h，因此默认配置下两者都需启用。裁剪 option 主要面向未来重构后的场景。

Windows 下还会自动链接 `user32 shell32 dwmapi gdiplus ws2_32`（style/reload/debug 依赖）。

## CMake 接入

在你的工程里把 MCDevTool 作为子目录引入后，链接 mcdk_core 即可获得全部能力：

```cmake
add_subdirectory(MCDevTool)  # 假设作为子模块

add_executable(my_launcher main.cpp)
target_compile_features(my_launcher PRIVATE cxx_std_23)
target_link_libraries(my_launcher PRIVATE mcdk_core)
# mcdk_core 已 PUBLIC 转发 mcdevtool + mcp + MCDEV_MOD_RESOURCE + include/modules 目录
```

## 快速开始

包含伞头即可获得启动器 API 与底层能力：

```cpp
#include <mcdk_core/core.h>
#include <iostream>
#include <filesystem>
```

### 方式一：强类型 config 启动（推荐，不依赖磁盘文件）

直接给 `LaunchConfig` 的字段赋值即可，字段名由编译器检查，IDE 可自动补全：

```cpp
int main() {
    mcdk::core::LaunchOptions options;

    // 强类型配置，按语义分组（点字段即可，无需知道任何 json 形状，IDE 可补全）
    options.config.gameExecutablePath    = "C:/path/to/Minecraft.Windows.exe";
    options.config.autoJoinGame          = true;
    options.config.world.folderName      = "MY_DEV_WORLD";
    options.config.world.gameMode        = 1;   // 0生存 1创造 2冒险
    options.config.player.name           = "dev";
    options.config.debug.includeDebugMod = true;

    // 服务 / 调试块同样是强类型
    options.config.mcpServer.enabled    = true;
    options.config.mcpServer.serverPort = 19133;
    options.config.debug.ptvsd.enabled  = true;

    options.printLogo = true;
    // 默认 configFilePolicy=ReadOnly + prompter=nullptr：不读不写磁盘 .mcdev.json，不弹交互
    return mcdk::core::Launcher::run(options);
}
```

> 核心模式约定：`gameExecutablePath` 必须填有效路径。若路径无效，默认（无 prompter）直接抛异常，
> 而不会像 CLI 那样弹 `(y/n)` 询问是否回写配置文件。

### 方式二：从磁盘 `.mcdev.json` 加载（CLI 兼容行为）

`loadConfigFromFile` 仍返回裸 json（等价原 `userParseConfig`：文件不存在时自动生成默认配置并写回），
再用 `LaunchConfig::fromJson` 适配为强类型。要完整复刻原 `mcdk.exe` 行为，需配上 `ReadWrite` 策略与 cin prompter：

```cpp
int main() {
    mcdk::core::LaunchOptions options;
    options.printLogo = true;
    options.config = mcdk::core::LaunchConfig::fromJson(
        mcdk::core::loadConfigFromFile(std::filesystem::current_path() / ".mcdev.json"));

    // 允许回写 .mcdev.json（游戏路径修复），并提供基于 std::cin 的交互回调
    options.configFilePolicy = mcdk::core::ConfigFilePolicy::ReadWrite;
    options.prompter = [](const std::string& question) -> bool {
        std::cout << question;
        std::string in;
        std::getline(std::cin, in);
        return in == "Y" || in == "y";
    };
    return mcdk::core::Launcher::run(options);
}
```

### 方式三：自定义日志输出（注入式 logger）

默认日志走内置 Win32 彩色输出（等价于原 `mcdk.exe`）。若需接入 GUI 日志框、文件日志等自定义体系，注入 `logger` 回调即可。设置后所有线程（含热更新 watcher、管道读取子线程）的输出都会转发：

```cpp
mcdk::core::LaunchOptions options;
options.logger = [](const std::string& msg, mcdk::ConsoleColor color) {
    // 业务自定义：写入文件、GUI、远程上报……
    std::cout << "[" << static_cast<int>(color) << "] " << msg << std::endl;
};
```

## API 参考

```cpp
namespace mcdk::core {

    // .mcdev.json 文件读写策略
    enum class ConfigFilePolicy {
        ReadOnly,     // 不读不写磁盘配置（默认）——核心模式
        ReadOrCreate, // 读，缺失则创建默认
        ReadWrite,    // 读 + 允许回写（如游戏路径修复）——CLI 行为
    };

    // 交互回调：返回 true 表示"同意/是"；nullptr 时核心模式遇交互场景直接抛异常
    using PromptCallback = std::function<bool(const std::string& question)>;

    // 启动选项
    struct LaunchOptions {
        LaunchConfig          config;                 // 强类型配置（见 launch_config.h）
        ConsoleOutputCallback logger   = nullptr;     // 日志回调，nullptr 用内置彩色输出
        bool                  printLogo = false;      // 是否打印 logo
        int                   pluginEnv = -1;         // -1=沿用环境变量，0/1 强制覆盖
        ConfigFilePolicy configFilePolicy = ConfigFilePolicy::ReadOnly; // 默认不碰磁盘
        PromptCallback        prompter = nullptr;     // 默认 nullptr=需交互时抛异常
    };

    // 一键启动，返回退出码（0 成功，1 异常）
    class Launcher {
    public:
        static int run(LaunchOptions options);
    };

    // 便捷函数（裸 json，主要供 CLI 兼容路径与 fromJson 配合）
    nlohmann::json loadConfigFromFile(const std::filesystem::path& configPath);
    nlohmann::json createDefaultConfig();
    void printLogo(bool pluginEnv);
}
```

### LaunchConfig 字段（`include/mcdk_core/launch_config.h`）

核心模式的配置已**按语义重新分组**为嵌套子结构，不再继承磁盘 `.mcdev.json` 的扁平布局。
所有有固定 schema 的块都是强类型；每个字段默认值与原 `.mcdev.json` 的 `config.value(key, default)` 默认值逐字段一致。

**顶层**

| 字段 | 类型 | 默认 | 对应 .mcdev.json |
|------|------|------|------------------|
| `gameExecutablePath` | `std::string` | `""`（必填） | `game_executable_path` |
| `autoJoinGame` | `bool` | `true` | `auto_join_game` |
| `includedModDirs` | `std::vector<ModDir{path,hotReload,enabled}>` | `[{"./"}]` | `included_mod_dirs` |

**`world`（世界）**

| 字段 | 类型 | 默认 | 对应 |
|------|------|------|------|
| `world.name` | `std::string` | `"MC_DEV_WORLD"` | `world_name` |
| `world.folderName` | `std::string` | `"MC_DEV_WORLD"` | `world_folder_name` |
| `world.seed` | `std::optional<uint64_t>` | 空=随机 | `world_seed` |
| `world.reset` | `bool` | `false` | `reset_world` |
| `world.type` / `world.gameMode` | `int` | `1` / `1` | `world_type` / `game_mode` |
| `world.enableCheats` / `keepInventory` / `doWeatherCycle` / `doDaylightCycle` | `bool` | `true` | 同名 |
| `world.experiments` | `std::optional<Experiments{...}>` | 空 | `experiment_options` |

**`player`（玩家）**

| 字段 | 类型 | 默认 | 对应 |
|------|------|------|------|
| `player.name` | `std::string` | `"developer"` | `user_name` |
| `player.skin` | `std::optional<SkinInfo{slim,skin}>` | 空=自动默认皮肤 | `skin_info` |

**`hotReload`（热更新）**

| 字段 | 类型 | 默认 | 对应 |
|------|------|------|------|
| `hotReload.mods` / `ui` / `shaders` / `materials` | `bool` | `true` / `false` / `false` / `false` | `auto_hot_reload_*` |

**`debug`（调试）**

| 字段 | 类型 | 默认 | 对应 |
|------|------|------|------|
| `debug.includeDebugMod` | `bool` | `true` | `include_debug_mod` |
| `debug.ptvsd` | `PtvsdDebugger{enabled,ip,port}` | `false / localhost / 56788` | `ptvsd_debugger` |
| `debug.modpc` | `ModpcDebugger{enabled,port}` | `false / 5632` | `modpc_debugger` |
| `debug.options` | `nlohmann::json` | `null` | `debug_options` |

**服务 / 外观（顶层）**

| 字段 | 类型 | 默认 | 对应 |
|------|------|------|------|
| `mcpServer` | `McpServer{enabled,serverIp,serverPort}` | `false / localhost / 19133` | `mcp_server_config` |
| `netease` | `NeteaseConfig{chatExtension}` | `false` | `netease_config` |
| `windowStyle` | `WindowStyle{...}` | 全空 | `window_style` |

> `debug.options` 是唯一保留 `nlohmann::json` 的块——它被原样注入调试 mod 的 `Config.py`，
> 键集由 python mod 自己定义、mcdk 不拥有其 schema，因此作为自由透传块而非强类型化。
>
> `LaunchConfig::fromJson(json)` / `LaunchConfig::toJson()` 负责"扁平 `.mcdev.json` ↔ 嵌套结构体"的互转
> （CLI 用 `fromJson`，磁盘格式不受结构分组影响）；对"全字段配置"可逐字段往返。环境变量
> （如 `MCDEV_PTVSD_*`、`MCDEV_MODPC_DEBUGGER_PORT`）的优先级仍高于配置，在启动时解析。

### 错误处理（结构化诊断，非抛异常）

适配层对**可容忍的局部异常**（典型：`included_mod_dirs` 里出现既非字符串、又非对象的非法元素）
**不抛异常、也不静默吞掉**，而是跳过该项并向出参追加一条**结构化诊断**——机器可判定，业务方
`switch(code)` 即可特化处理，**不需要解析文案**：

```cpp
struct LaunchConfig::Diagnostic {
    enum class Code { InvalidModDirElement /* 可扩展 */ };
    Code        code;     // 机器可判定的类别
    std::string pointer;  // RFC6901 JSON Pointer，如 "/included_mod_dirs/1"
    std::string detail;   // 补充（如实际 json 类型名），供展示/排查
    std::string message() const;  // 便利渲染，业务可无视
};

static LaunchConfig fromJson(const nlohmann::json& j,
                             std::vector<Diagnostic>* diagnostics = nullptr);
```

**程序化处理**（核心模式消费者）——按 `code` 分支，按 `pointer` 定位，不碰文案：

```cpp
std::vector<mcdk::core::LaunchConfig::Diagnostic> diags;
auto cfg = mcdk::core::LaunchConfig::fromJson(rawJson, &diags);
for (const auto& d : diags) {
    switch (d.code) {
    case mcdk::core::LaunchConfig::Diagnostic::Code::InvalidModDirElement:
        myReportInvalidModDir(d.pointer);   // 自行处理，无需解析字符串
        break;
    }
}
```

**仅展示**（CLI / 日志出口）渲染默认消息即可：`for (auto& d : diags) std::cerr << d.message();`

> 设计取向：**核心模式不应为这类小问题直接抛异常**（库 API 背着业务方崩溃不可接受），故走"跳过 + 结构化诊断"。
> 而字段类型彻底写错（如 `world_seed: "abc"`）这类无法容忍的错误仍由底层 nlohmann 抛出，交给调用方
> （CLI 的全局 `try/catch` 会捕获、记录并以退出码 1 终止；核心模式消费者按需自行 catch）。

## 环境变量

core 沿用原 mcdk 的环境变量语义，调用方可在启动前设置：

| 环境变量 | 作用 |
|----------|------|
| `MCDEV_IS_SUBPROCESS_MODE` | 子进程模式，跳过自动进入存档 |
| `MCDEV_AUTO_JOIN_GAME` | 0/1 覆盖自动进入游戏配置 |
| `MCDEV_IS_PLUGIN_ENV` | 标记插件环境，影响 logo 与 level.dat 更新策略 |
| `MCDEV_OUTPUT_MODE` | 1=伪终端输出模式 |
| `MCDEV_DEBUG_IPC_PORT` | IPC 端口（由 core 内部注入子进程） |

## 冒烟测试

`tests/core_smoke_test.cpp` 验证核心 API 可用性（不真正启动游戏），构建测试后运行：

```bash
cmake -B build -DMC_DEV_TOOL_BUILD_TEST=ON
cmake --build build
./build/tests/core_smoke_test
```

覆盖：① 空 config 抛异常；② 强类型字段流转抛异常；③ 注入式 logger；④ `fromJson`/`toJson`
全字段往返一致 + 缺省默认值与原 `.value(key,default)` 一致。预期输出：所有用例通过。

## 迁移说明（给 mcdk 维护者）

### v1（启动逻辑搬迁）
- 启动逻辑已从 `tools/mcdk/main.cpp` 整体搬迁至 `src/core.cpp`，**逐字搬迁，未改算法**。
- `main.cpp` 缩减为薄壳：控制台初始化 + CLI 拦截 + 读 `.mcdev.json` + 调 `Launcher::run`。
- `cli.cpp`（CLI 子命令）完全未动，仍通过 `argc > 1` 拦截。
- `modules/config.hpp` 的 `exit(1)` 改为抛 `std::runtime_error`。
- `modules/console.hpp` 补充 `#include <functional>`。

### v2（强类型 API + 策略化文件/交互）
- 新增 `include/mcdk_core/launch_config.h` + `src/launch_config.cpp`：强类型 `LaunchConfig` + `fromJson`/`toJson` 适配层。
- `LaunchOptions.config` 由裸 `nlohmann::json` 改为强类型 `LaunchConfig`；新增 `configFilePolicy` 与 `prompter`。
- **热路径 helper 改吃强类型**：`level.hpp`（`parseLevelOptionsFromUserConfig` / `createUserLevel`）、
  `env.hpp`（`getPtvsdConfigFromJson`→`getPtvsdConfig`）、`style_processor.hpp`（`UserStyleProcessor` ctor）、
  `mod_register.hpp`（`registerDebugMod` 改吃 `debug_options` 段）。`mcp_server.hpp` 删除 `getMcpServerConfigFromJson`
  （改由 core.cpp 从 `LaunchConfig::McpServer` 直接构造）。文件位置不变。
- `startGame` 签名 `(const nlohmann::json&)` → `(const LaunchConfig&, const LaunchOptions&)`；
  `launchGameExe` 的 `userConfig` 参数同改强类型。
- **唯一的 cin+写盘点**（游戏路径无效 y/n → `tryUpdateUserGamePath`）已策略化：交互走 `prompter`、
  回写仅在 `ConfigFilePolicy::ReadWrite` 时发生。`main.cpp` 注入 `ReadWrite` + cin prompter，CLI 行为零变更。
- 仅 `debug_options` 保留 `nlohmann::json`（mod 自定义 schema 的自由透传），其余配置块全强类型化。

## 后续规划（未在本期实现）

当前 `Launcher::run` 是一键全流程。未来可考虑拆分为 `prepareWorld()`（世界/Addon/dev_config 生成）与 `launchProcess()`（进程+服务编排）两段，供业务自定义中间环节。本期为保证零行为变更，保持单入口。
