# mcdk_core 核心模式库

mcdk_core 把原先写死在 `tools/mcdk/main.cpp` 里的启动逻辑，抽取成独立的可复用 STATIC 库。其他业务可以用几行代码 + 纯内存 config 启动完整的开发测试世界（含世界生成、Addon 注册、IPC、MCP、热更新、样式处理器），而**不必依赖磁盘上的 `.mcdev.json` 文件**。

## 设计目标

- 对外暴露纯 API（`mcdk::core::Launcher::run`），启动器逻辑与 `mcdk.exe` 解耦。
- 支持纯内存 config 入参，调用方自行构造 JSON 配置即可启动。
- 行为与原 `mcdk.exe` **完全一致**（启动逻辑逐字搬迁，零行为变更）。

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

### 方式一：纯内存 config 启动（推荐，不依赖磁盘文件）

```cpp
int main() {
    mcdk::core::LaunchOptions options;

    // 纯内存构造配置，结构同 .mcdev.json
    options.config = mcdk::core::createDefaultConfig();
    options.config["game_executable_path"] = "C:/path/to/Minecraft.Windows.exe";
    options.config["world_folder_name"]    = "MY_DEV_WORLD";
    options.config["auto_join_game"]       = true;
    options.config["include_debug_mod"]    = true;

    options.printLogo = true;
    return mcdk::core::Launcher::run(options);
}
```

### 方式二：从磁盘 `.mcdev.json` 加载（兼容旧行为）

```cpp
int main() {
    mcdk::core::LaunchOptions options;
    options.printLogo = true;
    options.config = mcdk::core::loadConfigFromFile(
        std::filesystem::current_path() / ".mcdev.json");
    return mcdk::core::Launcher::run(options);
}
```

`loadConfigFromFile` 等价于原 `userParseConfig`：文件不存在时自动生成默认配置并写回。

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

    // 启动选项
    struct LaunchOptions {
        nlohmann::json config = nlohmann::json::object();  // 纯内存配置
        ConsoleOutputCallback logger = nullptr;            // 日志回调，nullptr 用内置彩色输出
        bool printLogo = false;                            // 是否打印 logo
        int  pluginEnv = -1;                               // -1=沿用环境变量，0/1 强制覆盖
    };

    // 一键启动，返回退出码（0 成功，1 异常）
    class Launcher {
    public:
        static int run(LaunchOptions options);
    };

    // 便捷函数
    nlohmann::json loadConfigFromFile(const std::filesystem::path& configPath);
    nlohmann::json createDefaultConfig();
    void printLogo(bool pluginEnv);
}
```

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

预期输出：所有用例通过 (3/3)。

## 迁移说明（给 mcdk 维护者）

- 启动逻辑已从 `tools/mcdk/main.cpp` 整体搬迁至 `src/core.cpp`，**逐字搬迁，未改算法**。
- `main.cpp` 缩减为薄壳（约 40 行）：控制台初始化 + CLI 拦截 + 读 `.mcdev.json` + 调 `Launcher::run`。
- `cli.cpp`（CLI 子命令）完全未动，仍通过 `argc > 1` 拦截。
- `modules/config.hpp` 的 `exit(1)` 改为抛 `std::runtime_error`。
- `modules/mod_register.hpp` 的 `linkUserConfigModDirs` 新增可选 `ConsoleOutputCallback` 参数，默认 nullptr 静默；core 内部传入回调以保持原打印行为。
- `modules/console.hpp` 补充 `#include <functional>`。

## 后续规划（未在本期实现）

当前 `Launcher::run` 是一键全流程。未来可考虑拆分为 `prepareWorld()`（世界/Addon/dev_config 生成）与 `launchProcess()`（进程+服务编排）两段，供业务自定义中间环节。本期为保证零行为变更，保持单入口。
