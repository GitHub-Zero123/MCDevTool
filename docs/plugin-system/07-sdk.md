# 07 · SDK（用户侧）

上级索引：[README.md](README.md)

## 0. 两层的分界（务必读）

[02-abi-contract.md](02-abi-contract.md) 里「禁止 `std::string`」这类说法**只针对 `abi/` 那一层**，不针对 SDK。两者常被混为一谈，这里写死：

| 层 | 目录 | 能用 `std::string` / `std::vector` / `nlohmann::json` 吗 | 由谁编译 |
| --- | --- | :-: | --- |
| C ABI | `sdk/plugin-sdk/include/mcdk/plugin/abi/` | **不能**，纯 C99 | 两侧各编一次，必须字节级一致 |
| C++ SDK | 其余全部 | **能，随便用** | 随插件一起，与插件同编译器同 CRT |
| 插件作者代码 | 用户工程 | **能，随便用** | 同上 |

**SDK 存在的意义就是自动完成这层握手。** 插件作者从头到尾不会写出任何 `mcdk_` 前缀的 C 类型：

```cpp
// 用户写的
context.console().info("host " + std::string(context.hostVersion()));

// SDK 内部（console.hpp / detail/abi_bridge.hpp）
void Console::info(std::string_view message) const noexcept {
    mTable->log(mSelf, MCDK_LOG_INFO, detail::toAbi(message));  // ← 握手发生在这里
}
```

反方向同理：`context.hostVersion()` 返回的是 `std::string`，因为 ABI 给的 `mcdk_str` 是借用的、出了入口函数就失效，`Context::bindHost` 已经替用户做了深拷贝。

之所以能这么做，是因为 **SDK 随插件一起从源码编译**（§1 第 1 条）——SDK 与插件必然同编译器、同标准库、同 CRT，它们之间传 `std::string` 毫无问题。真正的边界在 SDK 与宿主之间，那里才只有 C。

构建期的两道闸门（[09-compatibility.md](09-compatibility.md) §4）也只作用于 `abi/` 层：`mcdk_abi_c99_check` 只编译那几个 C 头，SDK 的 C++ 代码不在它的输入里。

## 1. 设计约束

1. **SDK 禁止以预编译二进制分发**，必须随插件工程从源码构建。理由见 [01-overview.md](01-overview.md) §4.1。
2. **SDK 必须零第三方依赖。** nlohmann 支持通过 `MCDK_SDK_WITH_JSON` 可选开启（默认关闭，开启时由用户自备）。这样 `add_subdirectory` 用户与 xmake 用户（直接加 include 目录）都能接入。
3. **SDK 必须支持 `-fno-exceptions` 构建**，见 [02-abi-contract.md](02-abi-contract.md) §4.4。
4. SDK 要求的 C++ 标准**不得**高于 C++17，以免强制插件作者跟随宿主的 C++23。

## 2. 用户代码形态

用户只写这些，不接触任何 C 类型：

```cpp
#include <mcdk/plugin/plugin.hpp>

class MyPlugin final : public mcdk::Plugin {
    void onRegister(mcdk::Context& ctx) override {
        ctx.console().info("my-plugin 已加载");

        ctx.events().on<mcdk::ev::McpRegisterBefore>([&](const auto&) {
            ctx.mcp().addTool("my_tool", "工具说明", schema,
                [&ctx](const nlohmann::json& args) {
                    return ctx.game().executePython(args["code"], mcdk::Side::Server);
                });
        });

        ctx.events().on<mcdk::ev::GameLaunchFinish>([&](const auto& e) {
            ctx.console().print(mcdk::Color::Cyan,
                                "游戏 pid = " + std::to_string(e.pid));
        });

        ctx.events().on<mcdk::ev::LogLine>(
            mcdk::Dispatch::Sync,
            [](const auto& e) {
                return e.text.contains("spam") ? mcdk::EventResult::Stop
                                               : mcdk::EventResult::Continue;
            });
    }

    void onRuntime(mcdk::Context& ctx) override { /* ... */ }
};

MCDK_PLUGIN(MyPlugin, "com.example.my-plugin", "1.0.0")
```

## 3. `MCDK_PLUGIN` 宏的职责

1. 生成 `mcdk_plugin_entry` 导出函数；
2. 为本次加载 `new` 一个独占实例（`Context` + 插件对象 + 身份），指针放进 `mcdk_plugin_desc::user`，`on_unload` 时释放——**禁止**用静态单例，理由见 §5；
3. 填充 `mcdk_plugin_desc`：ABI 版本编译期写死，id / name / version 取自宏参数，但会被 `identity()` 的非空返回值覆盖；
4. 为每个虚函数生成 `noexcept` 静态蹦床，并装上异常屏障（见 [02-abi-contract.md](02-abi-contract.md) §4.2）；
5. 缓存各接口表指针并做 `struct_size` 能力探测。

这正是 godot-cpp 中 `GDCLASS` 宏的做法——其生成的 `notification_bind(GDExtensionClassInstancePtr p_instance, ...)` 内部 `reinterpret_cast` 回 C++ 对象再调虚函数，边界上只剩 `void*` 与函数指针。

## 4. CMake 集成

```cmake
# 用户工程 CMakeLists.txt
include(FetchContent)
FetchContent_Declare(mcdk_plugin_sdk
    GIT_REPOSITORY https://github.com/GitHub-Zero123/MCDevTool.git
    GIT_TAG        v1.x
    SOURCE_SUBDIR  sdk/plugin-sdk)
FetchContent_MakeAvailable(mcdk_plugin_sdk)

mcdk_add_plugin(my_plugin
    ID      com.example.my-plugin
    VERSION 1.0.0
    SOURCES src/main.cpp)
```

也支持直接 `add_subdirectory(third_party/mcdk-plugin-sdk)`，或安装后 `find_package(mcdk-plugin-sdk CONFIG)`。

### 4.1 `mcdk_add_plugin()` 的职责

| 项 | 行为 |
| --- | --- |
| 目标类型 | `MODULE` 库 |
| 可见性 | `CXX_VISIBILITY_PRESET hidden`、`VISIBILITY_INLINES_HIDDEN ON`，仅导出入口符号 |
| 产物布局 | 按 [06-loading.md](06-loading.md) §3.1 输出到 `MCDK_PLUGIN_OUTPUT_DIR` |
| 清单 | 依据 `ID` / `VERSION` 等参数生成 `plugin.json`；若工程已自带则校验一致性 |
| CRT 提示 | MSVC 上检测 `MSVC_RUNTIME_LIBRARY` 并打印说明：与宿主不一致是**允许**的，但前提是遵守 [02-abi-contract.md](02-abi-contract.md) §6 的内存规则 |
| 标准 | 默认 C++17，用户可自行提高 |

### 4.2 生成的 `.mcdev.json` 片段

由于插件不再被自动发现（见 [06-loading.md](06-loading.md) §1），`mcdk_add_plugin()` **应该**在构建后打印一段可直接粘贴的声明，降低接入摩擦：

```text
[mcdk] 插件已构建。将下面这段加入项目的 .mcdev.json：

  { "enable": true, "path": "D:/dev/my-plugin/build/plugins/my-plugin" }

  或执行： mcdk plugin add D:/dev/my-plugin/build/plugins/my-plugin
```

## 5. 加载器模式：一个 DLL 充当其他插件的宿主

这是 `config` 字段真正的用途，也是多实例与动态身份两项设计的由来。

原生插件天然不会被重复加载，真要那么做也有别的办法。但**绑定别的语言**不一样：一个 Python / Lua 宿主 DLL 只有一份，却要承载任意多个脚本。做法是把同一个 DLL 在 `plugins` 数组里声明多次，每条 `config` 指向不同脚本：

```jsonc
"plugins": [
    { "enable": true, "path": "./plugins/py-host/py_host.dll",
      "id": "com.me.py.hud",
      "config": { "id": "com.me.py.hud",   "name": "HUD",   "script": "hud/main.py" } },
    { "enable": true, "path": "./plugins/py-host/py_host.dll",
      "id": "com.me.py.timer",
      "config": { "id": "com.me.py.timer", "name": "Timer", "script": "timer/main.py" } }
]
```

要让这条路成立，两件事必须同时为真，缺一不可：

**其一，每条声明必须有独立实例。** SDK **禁止**把插件状态放在静态变量里——那样第二次加载会把第一次的 `Context` 与 `config` 整个覆盖掉。实例由 `MCDK_PLUGIN` 宏在入口里 `new` 出来，指针放进 `mcdk_plugin_desc::user` 交给宿主，宿主原样回传给每个回调，`on_unload` 里释放。这正是 `user` 字段存在的意义。

**其二，每个实例必须能报出自己的身份。** 重写 `Plugin::identity()`，它在任何阶段回调之前被调用，此时 `config` 已可读：

```cpp
mcdk::PluginIdentity identity(mcdk::Context& context) override {
    const auto script = parseScript(context.configJson());
    return {.id = "com.me.py." + script, .name = script};   // version 留空则沿用宏里的值
}
```

不这么做，五个脚本都会报出宿主 DLL 编译期写死的那个 id，后果是：日志分不清是哪个脚本打的、声明里的 `id` 防替换校验形同虚设、将来的重名检测与依赖解析一并失效。

**失败是按实例隔离的。** 某个脚本加载不起来，只有那一条声明对应的实例在 REGISTER 阶段失败并被卸载，同一个 DLL 的其余实例照常运行。

完整可运行的示例见 `sdk/plugin-sdk/examples/02-loader/`，回归测试见 `tests/plugin_loader_pattern_test.cpp`。

## 6. 示例工程

| 目录 | 用途 |
| --- | --- |
| `examples/00-abi-conformance/` | ABI 一致性测试插件，见 [09-compatibility.md](09-compatibility.md) §2 |
| `examples/01-hello/` | 最小插件：注册、打日志、读 config |
| `examples/02-loader/` | 加载器模式：一个 DLL 充当其他插件的宿主，见 §5 |
| `examples/03-mcp-tool/` | 注册一个 MCP 工具并调用游戏内 Python（待 `mcdk.mcp` 开放） |
| `examples/04-hotreload/` | 注册自定义 watcher（待 `mcdk.hotreload` 开放） |
| `templates/plugin-template/` | 供用户复制的起步工程，含 `CMakeLists.txt` 与 `plugin.json` |
