# 02 · ABI 兼容性契约（规范）

上级索引：[README.md](README.md)

本文档是整套设计的核心。[03-abi-reference.md](03-abi-reference.md) 及之后的所有接口定义都必须服从本文。

## 1. 兼容性目标矩阵

宿主当前以 MSVC + 静态 CRT（`MultiThreaded`）构建。插件**必须**能在以下任一组合下构建并被同一宿主二进制加载：

| 编译器 | 标准库 | CRT | 异常模型 | 要求 |
| --- | --- | --- | --- | --- |
| MSVC 2022 | MSVC STL | `/MT` | `/EHsc` | 必须 |
| MSVC 2022 | MSVC STL | `/MD` | `/EHsc` | 必须 |
| MSVC 2019 | MSVC STL | `/MD` | `/EHsc` | 必须 |
| clang-cl | MSVC STL | `/MD` | `/EHsc` | 必须 |
| MinGW-w64 GCC | libstdc++ | msvcrt/UCRT | SJLJ/DWARF/SEH | 必须 |
| clang++ (MinGW) | libc++ | UCRT | SEH | 应该 |
| 任意 | 任意 | 任意 | `-fno-exceptions` | 必须（见 §4.4） |

额外要求：宿主与插件的 C++ 标准版本**可以**不同（宿主 C++23，插件 C++17 亦须可用）；宿主与插件的指针宽度**必须**相同，这一点由操作系统加载器保证，无需额外校验。

## 2. 边界消化原则

> **凡是编译器相关的 C++ 机制，必须由拥有它的那一侧在自己的边界内终结，绝不允许跨越 ABI。**

"拥有它的那一侧"指该机制的代码所在的二进制模块。因此每个机制都有两个对称的消化点：

- 插件侧的机制由 **SDK** 消化（SDK 与插件同编译器，可自由处理）；
- 宿主侧的机制由 **plugin-host 的 shim 层**消化（shim 与宿主同编译器，可自由处理）。

ABI 边界上剩下的只有：定宽整数、指针、以及 `MCDK_CALL` 标注的函数指针。

## 3. 消化清单（规范）

| C++ 机制 | 跨界形态 | 插件侧消化点 | 宿主侧消化点 |
| --- | --- | --- | --- |
| 异常 | `mcdk_status` + 借用错误串 | SDK 回调蹦床 `try/catch(...)` | shim 函数 `try/catch(...)` |
| `std::string` / `string_view` | `mcdk_str`（UTF-8，借用） | SDK 转换 | shim 转换 |
| `std::vector<T>` / 容器 | 回调式枚举或 `(ptr, count)` 只读视图 | SDK 收集成容器 | shim 展开 |
| `std::function` / lambda | `fn_ptr + void* userdata` | SDK 持有闭包，导出静态蹦床 | 同左 |
| 继承 + 虚函数 | 函数指针表（POD struct） | `MCDK_PLUGIN` 宏生成蹦床 | 不适用 |
| `std::filesystem::path` | `mcdk_str`（UTF-8 generic 形式） | SDK 转换 | shim 用 `Utils::pathToGenericUtf8` |
| `std::shared_ptr` / RAII | `mcdk_handle` + 显式 `create`/`destroy` | SDK 用 RAII 包住句柄 | shim 维护句柄表 |
| 模板 | 无（边界上只有具体签名） | SDK 内实例化 | 不适用 |
| RTTI / `dynamic_cast` / `typeid` | 无（禁止） | 显式类型 tag | 显式类型 tag |
| `new` / `delete` | 无（禁止跨界配对） | 见 §6 | 见 §6 |
| `nlohmann::json` | `mcdk_str`（JSON 文本） | SDK 可选提供 json 重载 | shim 解析/序列化 |
| iostream / `FILE*` / `errno` / locale | 无（禁止） | — | — |
| `std::chrono` 时间点/时长 | `int64_t` 毫秒 | SDK 转换 | shim 转换 |

## 4. 异常消化

### 4.1 总规则

**规范：任何 C++ 异常都禁止穿越 ABI 边界。** 边界两侧的每一个函数指针实现都必须是 `noexcept` 的 C 蹦床，内部以 `try/catch(...)` 兜底。

这一条对本项目尤其关键：宿主现有代码大量使用异常（`startGame()` 在游戏路径无效、皮肤文件缺失时直接 `throw std::runtime_error`），这些异常绝不能穿过 C 边界进入插件的栈帧。异常应当少用，但不可能 100% 避免——所以策略不是消灭它，而是**在边界内侧重新消化掉**。

### 4.2 方向一：插件 → 宿主

插件作者代码抛出的异常，在 SDK 的蹦床里被吃掉，转成状态码交给宿主。

```cpp
// sdk/plugin-sdk/include/mcdk/plugin/detail/barrier.hpp
namespace mcdk::detail {

    // 错误槽位于插件自己的 DLL，线程局部，不跨界
    struct ErrorSlot {
        mcdk_status code = MCDK_OK;
        std::string message;
    };
    ErrorSlot&  errorSlot() noexcept;
    mcdk_status setError(mcdk_status code, const char* message) noexcept;

    // 所有交给宿主的回调都必须经过它
    template <class F>
    auto guard(F&& fn, std::invoke_result_t<F> onError) noexcept -> std::invoke_result_t<F> {
#if MCDK_SDK_HAS_EXCEPTIONS
        try {
            return fn();
        } catch (const mcdk::Error& e) {
            setError(e.status(), e.what());
        } catch (const std::bad_alloc&) {
            setError(MCDK_ERR_OUT_OF_MEMORY, "plugin: bad_alloc");
        } catch (const std::exception& e) {
            setError(MCDK_ERR_PLUGIN_EXCEPTION, e.what());
        } catch (...) {
            setError(MCDK_ERR_PLUGIN_EXCEPTION, "plugin: unknown exception");
        }
        return onError;
#else
        return fn();   // -fno-exceptions 构建：屏障退化为直通，见 §4.4
#endif
    }

} // namespace mcdk::detail
```

事件回调的蹦床形态：

```cpp
template <class Event, class Handler>
static mcdk_event_result MCDK_CALL trampoline(const mcdk_event* ev, void* user) noexcept {
    return detail::guard(
        [&] { return toAbi((*static_cast<Handler*>(user))(Event::from(ev))); },
        MCDK_EVENT_CONTINUE);   // 插件抛异常 → 记录并当作"不干预"，绝不影响宿主流程
}
```

失败语义**必须**逐个回调明确定义，不允许由实现随意决定：

| 回调 | 抛异常时的等效行为 |
| --- | --- |
| 普通事件处理器 | 记为 `CONTINUE`，不中断其他插件 |
| 可否决事件处理器 | 记为 `CONTINUE`。**禁止**理解为否决，否则一个 bug 会让游戏起不来 |
| MCP 工具 / RPC 处理器 | 转为该协议的错误响应，错误串取自错误槽 |
| `on_stage(REGISTER)` | 该插件加载失败，已注册项全部回滚 |
| `on_stage(CONFIG/WORLD/RUNTIME)` | 该插件标记为降级并记录，其余插件与主流程不受影响 |
| `on_unload` | 记录日志后忽略，卸载流程继续 |

### 4.3 方向二：宿主 → 插件

宿主侧对称处理。每个接口函数都是 `noexcept` shim：

```cpp
// tools/mcdk/src/plugin_host/interfaces/game.cpp
static mcdk_status MCDK_CALL game_execute_python(
    mcdk_handle self, mcdk_str code, uint32_t side, mcdk_str* out_json) noexcept
{
    return host::guard(self, [&] {
        auto& session = host::sessionOf(self);
        // 下面这段是现有实现，可以自由抛 std::runtime_error
        auto value = mcdk::ipc_code_execution::requestCodeReturnValueJson(
            session.ipcServer(), host::toString(code), side == MCDK_SIDE_CLIENT);
        *out_json = host::stashResult(self, value.dump());   // 存入调用方专属槽，见 §7
        return MCDK_OK;
    });
}
```

`host::guard` 与 SDK 侧的 `detail::guard` 结构一致，把 `std::runtime_error`、`std::filesystem::filesystem_error`、`nlohmann::json::exception` 等映射到稳定的 `mcdk_status`，并把 `what()` 存入该插件的错误槽。

**规范：`tools/mcdk/src/plugin_host/interfaces/` 下的每一个导出函数指针都必须经过 `host::guard`，没有例外。** 该规则由 CI 静态检查，见 [09-compatibility.md](09-compatibility.md) §5。

### 4.4 `-fno-exceptions` 插件

插件**可以**在关闭异常的情况下构建。此时 SDK 以 `MCDK_SDK_HAS_EXCEPTIONS=0` 编译，屏障退化为直通调用，SDK 只暴露返回 `std::expected` 的 `tryXxx` 形式 API。

这不影响 ABI：屏障是否存在只改变插件内部行为，边界上的函数签名完全相同。宿主无需知道插件是否开启异常。

### 4.5 SDK 双风格 API

同一个能力提供两种调用风格，编译期开关决定可用性，**两者编译出的 ABI 调用完全相同**：

```cpp
ctx.game().executePython(code);              // 失败抛 mcdk::Error，需 MCDK_SDK_HAS_EXCEPTIONS
auto r = ctx.game().tryExecutePython(code);  // 返回 std::expected<json, mcdk::Error>，永远可用
```

### 4.6 硬件异常不在本节范围

`catch(...)` 在 `/EHsc` 下**不捕获** SEH（访问违例、除零）。插件内的段错误会直接穿过宿主栈帧。v1 的处理是：宿主在调用插件的最外层可选加装 SEH 过滤器，**仅用于生成指明肇事插件 ID 的崩溃报告**，随后终止进程。**禁止**在捕获 SEH 后继续运行——此时进程状态已不可信。

## 5. 类型与布局规则（规范）

1. **调用约定必须显式标注。** 所有 ABI 函数指针必须带 `MCDK_CALL`。

   ```c
   #if defined(_WIN32) && !defined(_WIN64)
   #  define MCDK_CALL __cdecl
   #else
   #  define MCDK_CALL
   #endif
   ```

2. **禁止使用 C++ `bool`**，用 `typedef uint8_t mcdk_bool;`，取值只能是 0 或 1。

3. **禁止使用 `typedef enum`。** 枚举的底层类型在不同编译器/选项下可能收缩到 `char` 或扩展到 `int`。一律写成定宽整数 + 匿名枚举常量：

   ```c
   typedef uint32_t mcdk_log_level;
   enum { MCDK_LOG_TRACE = 0, MCDK_LOG_DEBUG = 1, MCDK_LOG_INFO = 2,
          MCDK_LOG_WARN  = 3, MCDK_LOG_ERROR = 4 };
   ```

4. **禁止位域。** 位域的分配顺序和跨单元填充是实现定义的。用显式掩码常量。

5. **禁止 `#pragma pack`。** 所有结构体字段按自然对齐排布，必要时**显式**写出 `uint32_t _reserved;` 占位，不依赖编译器补洞。

6. **禁止 `long`、`unsigned long`、`wchar_t`。** 前两者在 Windows 是 4 字节、LP64 是 8 字节；`wchar_t` 在 MSVC 是 16 位、GCC 是 32 位。一律用 `<stdint.h>` 定宽类型。`size_t` **可以**使用（宿主与插件指针宽度必然相同）。

7. **复合类型禁止按值传递或返回。** 所有 ABI 函数的返回值只能是定宽标量或指针；复合体一律通过 `out` 指针参数返回。小结构体的按值传递/返回规则在各平台 ABI 中最容易出分歧，直接回避。

8. **所有可增长结构体的首字段必须是 `uint32_t struct_size`。** 填充方按自己版本的 `sizeof` 写入，读取方据此判断字段是否存在。这正是 Godot 4.0 的 `GDExtensionInterface` 所缺、并最终导致其在 4.1 被整体推翻的关键字段。

9. **结构体字段只增不改。** 禁止删除字段、禁止改变字段类型、禁止调整字段顺序、禁止复用已废弃字段的位置。废弃字段保留原位，填 `NULL` 或 0。

10. **枚举常量值只增不改。** 已分配的数值永不复用。

11. **ABI 头必须是纯 C99**，可被 C 编译器直接包含，除 `<stdint.h>` / `<stddef.h>` 外无任何依赖。禁止在 ABI 头中出现访问宿主状态的 `static inline` 函数（自洽的纯计算小函数除外）。

12. **符号可见性。** 插件必须以 `hidden` 为默认可见性，只导出唯一入口符号。在类 Unix 平台宿主必须以 `RTLD_LOCAL` 加载，避免插件与宿主各自静态链接的同名第三方库（nlohmann、asio）发生符号插入。

13. **静态初始化期禁止访问宿主。** SDK 必须保证所有接口指针在入口函数被调用前为 `NULL`，插件的全局对象构造期不得调用任何 `ctx.*`。

14. **所有 `mcdk_str` 必须是合法 UTF-8。** 路径统一用 generic 形式（`Utils::pathToGenericUtf8`）。游戏日志的实际编码取决于 Python Mod 的输出与运行环境，**转码与校验的责任在宿主 shim**：禁止把未经校验的原始字节直接塞进 `mcdk_str`，非法字节序列必须替换而非透传。插件侧对收到的 `mcdk_str` 可以直接按 UTF-8 处理，不必自行防御。

## 6. 内存与所有权

**规范：谁分配谁释放。跨界的内存所有权转移必须显式，且只允许以下三种形态。**

| 形态 | 语义 | 用途 |
| --- | --- | --- |
| **借用（默认）** | 指针在被调函数返回前有效，调用方如需保留必须立即深拷贝 | 绝大多数入参、错误串、事件 payload、枚举回调 |
| **调用方缓冲** | 调用方给 `(buf, cap)`，被调方写入并回填所需长度；`cap` 不足时只回填长度、不写入、返回 `MCDK_ERR_BUFFER_TOO_SMALL` | 变长数据，调用方能先查询大小 |
| **宿主持有句柄** | 宿主分配并持有，交给调用方一个 `mcdk_handle`；调用方查询大小、用自备缓冲拷走内容，再显式 `release` | 大块二进制，如窗口截图 |

**禁止**在一侧 `new`/`malloc` 而在另一侧 `delete`/`free`。这是 `/MT` 宿主 + `/MD` 插件组合下最典型的崩溃来源，`components/profiler/tracy-bridge` 的 CRT 注释已经记录过同类教训。

上述三种形态的共同性质：**v1 的 ABI 边界上没有任何分配器穿越。** 每一侧都只释放自己分配的内存，宿主持有的资源由宿主自己 `release`。因此 v1 **不提供** `mcdk.mem` 一类的共享分配器接口——只要能靠句柄 + 调用方缓冲解决，就不引入分配器，这是最彻底的 CRT 隔离。

事件 payload 在 `MCDK_DISPATCH_QUEUED` 模式下由宿主深拷贝后入队，回调返回即失效；在 `MCDK_DISPATCH_SYNC` 模式下为发射方栈上对象的借用。两种模式下插件都**禁止**保存 payload 指针。

## 7. 错误传递

错误消息采用**被调方 TLS 暂存 + 调用方立即拷贝**，完全回避所有权问题：

```c
/* 统一放在 mcdk_iface_core 里 */
void MCDK_CALL (*get_last_error)(mcdk_handle self, mcdk_str* out_message);
```

语义：`out_message` 指向**被调方**线程局部缓冲，仅在下一次同线程 ABI 调用前有效，调用方必须立即拷贝。双方都不分配、都不释放。

状态码分段：

```c
typedef int32_t mcdk_status;
enum {
    MCDK_OK                    =  0,
    MCDK_ERR_INVALID_ARGUMENT  = -1,
    MCDK_ERR_INVALID_HANDLE    = -2,
    MCDK_ERR_NOT_SUPPORTED     = -3,   /* 该宿主版本没有此能力 */
    MCDK_ERR_WRONG_STAGE       = -4,   /* 在错误的生命周期阶段调用 */
    MCDK_ERR_OUT_OF_MEMORY     = -5,
    MCDK_ERR_TIMEOUT           = -6,
    MCDK_ERR_GAME_NOT_READY    = -7,
    MCDK_ERR_DUPLICATE         = -8,
    MCDK_ERR_BUFFER_TOO_SMALL  = -9,
    MCDK_ERR_PLUGIN_EXCEPTION  = -100, /* 插件侧屏障捕获 */
    MCDK_ERR_HOST_EXCEPTION    = -101, /* 宿主侧屏障捕获 */
};
```

## 8. 版本协商与演进

三级版本机制，缺一不可：

1. **ABI 主次版本。** `MCDK_ABI_VERSION_MAJOR` 不一致直接拒绝加载并打印可读原因；`plugin.minor > host.minor` 时拒绝并提示升级 mcdk；`plugin.minor <= host.minor` 放行。
2. **接口版本号。** `get_interface(name, version)` 的 `version` 仅用于整体断代（`mcdk.game/1` 与 `mcdk.game/2` 可并存）。正常演进**禁止**升此号。
3. **`struct_size` 渐进增长。** 日常新增能力一律靠往接口表尾部追加字段，配合 `struct_size` 做能力探测。

**`MCDK_ABI_VERSION_MINOR` 的递增时机（规范）：新增接口表、或往已有接口表追加字段，都必须 +1。**

minor 与 `struct_size` 不是二选一，而是互补：`struct_size` 让插件在**运行期**探测单个字段是否存在；minor 让宿主在 **`LoadLibrary` 之前**就能拒绝"要求新能力的插件跑在旧宿主上"，并给出"请升级 mcdk 到 x.y"这样的可读提示，而不是等插件跑到一半才发现某个函数指针不存在。缺了前者无法优雅降级，缺了后者只能给出一堆莫名其妙的 `MCDK_ERR_NOT_SUPPORTED`。

演进纪律借鉴 Godot——其 `gdextension_interface.h` 中 168 个接口函数全部带如下注释，其中 9 个已弃用但仍保留可用：

```c
/**
 * @name execute_python
 * @since 1.0
 * @deprecated 1.3，改用 execute_python2。
 */
```

`@name` / `@since` / `@deprecated` 为**规范要求**，格式必须机器可解析，CI 据此生成兼容性报告。签名需要变更时**禁止**原地修改，必须在表尾追加带数字后缀的新字段（`execute_python2`），旧字段保留并由 shim 转发。

SDK 侧把探测封装掉，用户不接触 `struct_size`：

```cpp
if (ctx.game().has<&mcdk_iface_game::capture_window>()) { ... }
```

## 9. 禁止事项速查

跨越 ABI 边界时，以下一律禁止：

C++ 异常穿越 · C++ 类型（`std::*`、自定义类、引用） · 继承与虚表 · RTTI / `typeid` / `dynamic_cast` · 模板 · `new`/`delete` 跨侧配对 · `FILE*` / 文件描述符 / `errno` / locale / iostream · `long` / `unsigned long` / `wchar_t` / C++ `bool` · `typedef enum` · 位域 · `#pragma pack` · 复合类型按值传递或返回 · 静态初始化期访问宿主 · 保存借用指针
