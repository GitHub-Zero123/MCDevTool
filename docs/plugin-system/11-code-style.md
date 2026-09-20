# 11 · 代码风格

上级索引：[README.md](README.md)

## 1. 结论

**两层，两套风格，风格分界线就是 ABI 边界。**

| 层 | 风格 | 谁会看到 |
| --- | --- | --- |
| `abi/**.h` | C 风格，`mcdk_snake_case` | 只有宿主 shim 与 SDK 内部 |
| SDK C++ 封装、`plugin-host` C++ 实现 | 与 `src/` 完全一致：类 PascalCase，方法 camelCase | 插件作者、宿主开发者 |

插件作者从头到尾只写第二层。一个典型插件的源码里不会出现任何 `mcdk_` 前缀的标识符——`MCDK_PLUGIN` 宏与 `ctx.xxx()` 把 C 层整个挡住了。

所以**不割裂**：在人要读要写的地方，风格和 `src/` 一模一样。

## 2. 现有 `src/` 风格（实测）

`.clang-tidy` 未启用 `readability-identifier-naming`，以下是从现有代码归纳的事实约定：

| 元素 | 约定 | 实例 |
| --- | --- | --- |
| 顶层命名空间 | 小写 | `mcdk`（新代码主流，39 处）；`MCDevTool` / `MCDevLink` 为较早的顶层命名空间 |
| 子命名空间 | **snake_case** | `mcdk::performance`、`mcdk::mcp_tool_definitions`、`mcdk::shader_reload_support` |
| 类 / 结构体 | PascalCase | `RpcRegistry`、`ConsoleWatcherTask`、`HostBridgeTask`、`CaptureOptions` |
| 方法 / 自由函数 | camelCase | `bindRaw`、`setOutputCallback`、`pathToGenericUtf8`、`startGame` |
| 数据成员 | `m` + PascalCase | `mMutex`、`mMethods`、`mSealed`、`mOutputCallback` |
| 枚举类型 | PascalCase | `ConsoleColor`、`RpcMode`、`GameLogProtocol` |
| 枚举值 | PascalCase | `ConsoleColor::DarkGray`、`RpcMode::Notification` |

两处偏差，新代码**应该**统一到主流写法：

- `NativeBridgeLoader` 用 `impl_` 尾下划线，其余全部用 `mImpl` / `m` 前缀；
- 子命名空间是 snake_case 而非 PascalCase，与顶层的 `MCDevTool` 不一致，但既已成主流，插件系统跟随 snake_case。

## 3. C ABI 层风格（规范）

| 元素 | 约定 | 实例 |
| --- | --- | --- |
| 类型 | `mcdk_` + snake_case | `mcdk_str`、`mcdk_status`、`mcdk_session_info` |
| 接口表类型 | `mcdk_iface_` + 模块名 | `mcdk_iface_console`、`mcdk_iface_game` |
| 事件 payload 类型 | `mcdk_ev_` + 事件名 | `mcdk_ev_game_launch_finish` |
| 函数指针 typedef | `mcdk_` + snake_case | `mcdk_log_sink`、`mcdk_get_interface_fn` |
| 结构体字段 | snake_case | `struct_size`、`game_ipc_port` |
| 枚举常量 | `MCDK_` + UPPER_SNAKE | `MCDK_COLOR_DARK_GRAY`、`MCDK_ERR_TIMEOUT` |
| 宏 | `MCDK_` + UPPER_SNAKE | `MCDK_CALL`、`MCDK_ABI_VERSION_MAJOR` |
| 导出符号 | `mcdk_` + snake_case | `mcdk_plugin_entry` |

## 4. 为什么 C 层不跟 `src/` 统一

三条理由，按重要性排序：

**其一，仓库里已经有 C ABI 惯例，且就是 snake_case。** `tools/mcdk_api/include/mcdk_api.h` 导出的是 `mcdk_api_get_game_exe_paths`。插件 ABI 若改用 camelCase，反而会和项目自己的另一个 C 头文件割裂。

**其二，ABI 头是给机器读的。** 按 [02-abi-contract.md](02-abi-contract.md) §5.11 它必须能被 C99 编译器单独编译，其直接消费者除了宿主和 SDK，还包括将来可能出现的绑定生成器（Rust `bindgen`、Python `ctypes`、C# P/Invoke）。那些生态默认按 snake_case C API 的形态来处理。

**其三，风格差异在这里承担了"边界可见"的功能。** 看到 `mcdk_str` 就知道这是 ABI 类型、是借用的、不能存；看到 `mcdk::Console` 就知道是 SDK 包装、可以随便用。在一份同时涉及两层的文件（shim 实现）里，这种视觉区分能直接减少把借用指针存下来这类错误。统一风格反而会让边界隐形。

Godot 采取的是同一分法：`gdextension_interface.h` 是 C 风格，godot-cpp 是 Godot 自己的 C++ 风格，两者从不混用。

## 5. 对照示例

同一个能力在两层的形态：

```c
/* abi/iface/console.h —— C 层 */
typedef uint32_t mcdk_color;
enum { MCDK_COLOR_DEFAULT = 0, MCDK_COLOR_CYAN = 5, MCDK_COLOR_DARK_GRAY = 10 };

typedef struct mcdk_iface_console {
    uint32_t struct_size;
    void MCDK_CALL (*log_colored)(mcdk_handle self, mcdk_color color, mcdk_str message);
} mcdk_iface_console;
```

```cpp
// include/mcdk/plugin/console.hpp —— C++ 层，插件作者看到的
namespace mcdk {

    enum class Color { Default, Green, Red, Blue, Yellow, Cyan,
                       Magenta, White, Black, Gray, DarkGray };

    class Console {
    public:
        void print(Color color, std::string_view message) const;
        void info(std::string_view message) const;
        void warn(std::string_view message) const;
        void error(std::string_view message) const;

    private:
        mcdk_handle                 mSelf = 0;
        const mcdk_iface_console*   mTable = nullptr;
    };

} // namespace mcdk
```

注意 `Color` 与 `mcdk_color` 是两个独立枚举，转换必须走显式 `switch`——理由见 [05-interfaces.md](05-interfaces.md) §4.2。

## 6. 宿主 shim 层

`components/plugin-host/src/interfaces/` 同时触碰两层，规约：

- 文件内的 C 函数实现命名为 `<模块>_<动作>`，与 ABI 表的字段名一致：`console_log_colored`、`game_execute_python`；
- 这些函数一律 `static` + `noexcept`，只在文件末尾组装成接口表；
- 函数体内立刻转成 C++ 形态，此后全部 `src/` 风格。

```cpp
// components/plugin-host/src/interfaces/console.cpp
static void MCDK_CALL console_log_colored(
    mcdk_handle self, mcdk_color color, mcdk_str message) noexcept
{
    host::guard(self, [&] {
        host::sessionOf(self).printColored(toConsoleColor(color), host::toString(message));
    });
}

const mcdk_iface_console kConsoleTable = {
    .struct_size = sizeof(mcdk_iface_console),
    .log         = console_log,
    .log_colored = console_log_colored,
};
```

## 7. 格式化

沿用仓库根目录的 `.clang-format`，ABI 头与 SDK 一并纳入。

是否在 `.clang-tidy` 中启用 `readability-identifier-naming` 把上述约定变成强制，见 [10-roadmap.md](10-roadmap.md) §4 待定问题 8——它会同时对存量代码生效，需要先评估改动面。
