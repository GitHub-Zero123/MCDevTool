# 03 · ABI 定义

上级索引：[README.md](README.md)　前置阅读：[02-abi-contract.md](02-abi-contract.md)

本文所有定义都必须服从 [02-abi-contract.md](02-abi-contract.md) 的规范条目。

## 1. 基础类型

```c
/* abi/core.h —— 纯 C99 */
#include <stddef.h>
#include <stdint.h>

#define MCDK_ABI_VERSION_MAJOR 1
#define MCDK_ABI_VERSION_MINOR 0

#if defined(_WIN32) && !defined(_WIN64)
#  define MCDK_CALL __cdecl
#else
#  define MCDK_CALL
#endif

typedef uint8_t  mcdk_bool;     /* 只能是 0 或 1 */
typedef int32_t  mcdk_status;   /* 0 = MCDK_OK，负数为错误 */
typedef uint64_t mcdk_handle;   /* 不透明句柄，0 表示无效 */

/* UTF-8 字符串视图，不保证 NUL 结尾，默认借用语义 */
typedef struct mcdk_str {
    const char* ptr;   /* len == 0 时可为 NULL */
    size_t      len;   /* 字节数，不含结尾 NUL */
} mcdk_str;
```

状态码常量见 [02-abi-contract.md](02-abi-contract.md) §7。

## 2. 入口点

唯一导出符号，签名固定。形态借鉴 GDExtension 的"填充初始化结构体"，避免入口内的重入问题：

```c
/* abi/entry.h */
typedef const void* (MCDK_CALL *mcdk_get_interface_fn)(const char* name, uint32_t version);

typedef struct mcdk_host_info {
    uint32_t              struct_size;
    uint32_t              abi_major;
    uint32_t              abi_minor;
    uint32_t              _reserved;
    mcdk_str              host_version;     /* 例如 "1.4.2" */
    mcdk_handle           self;             /* 本插件实例句柄，所有调用都要带上 */
    mcdk_get_interface_fn get_interface;
} mcdk_host_info;

typedef struct mcdk_plugin_desc {
    uint32_t  struct_size;
    uint32_t  abi_major;        /* SDK 编译期写死，宿主校验 */
    uint32_t  abi_minor;
    uint32_t  min_stage;        /* 最低所需阶段，语义同 GDExtension 的 minimum_initialization_level */
    mcdk_str  id;
    mcdk_str  name;
    mcdk_str  version;
    void*     user;
    mcdk_status (MCDK_CALL *on_stage)(void* user, mcdk_stage stage); /* 返回值不可省略 */
    void (MCDK_CALL *on_unload)(void* user);
} mcdk_plugin_desc;

MCDK_PLUGIN_EXPORT mcdk_bool MCDK_CALL
mcdk_plugin_entry(const mcdk_host_info* host, mcdk_plugin_desc* out_desc);
```

`on_stage` **必须**返回 `mcdk_status`，不可改成 `void`：插件侧的异常被 SDK 屏障就地吃掉之后，宿主没有别的途径知道该阶段是否失败，而 [02-abi-contract.md](02-abi-contract.md) §4.2 要求 REGISTER 阶段失败时卸载该插件、其余阶段失败时降级——两者都得拿得到结果。`on_unload` 保持 `void`，卸载失败没有可采取的补救动作。

入口符号名写在插件清单的 `entry_symbol` 字段中，默认 `mcdk_plugin_entry`，**可以**自定义（同一动态库承载多个插件时需要）。详见 [06-loading.md](06-loading.md)。

## 3. 接口查询

`get_interface(name, version)` 返回指向**常量接口表**的指针，生命周期与宿主进程相同。

```c
typedef struct mcdk_iface_console {
    uint32_t struct_size;
    void MCDK_CALL (*log)(mcdk_handle self, mcdk_log_level level, mcdk_str message);
    void MCDK_CALL (*log_colored)(mcdk_handle self, mcdk_color color, mcdk_str message);
    /* 新字段只能追加到这里 */
} mcdk_iface_console;
```

### 3.1 选型说明（与 Godot 的差异）

Godot 4.1 起用 `get_proc_address(const char*)` 逐函数查名（4.0 时本是单个巨型 struct，因无法演进而被整体推翻）。本设计改为**按模块分表 + `struct_size`**，理由：

| | Godot（逐函数） | 本设计（分模块表） |
| --- | --- | --- |
| 插件生命周期 | 第三方预编译二进制，可能数年不重编，需跨大版本运行 | 跟随 mcdk 版本，随版本重编 |
| 接口规模 | 168 个函数 | 预计数十个 |
| 绑定代码来源 | `extension_api.json` + `binding_generator.py` 自动生成 | 手写 |
| 主要风险 | —— | 手写 168 对"名字 ↔ 函数指针 typedef"时，名字打错得到 `NULL`、类型转错是静默 UB，两者都没有编译期保护 |

Godot 抛弃的是**单个无 `struct_size` 字段的巨型 struct**，而非按模块切分的小表。补上 `struct_size` 后，分模块表不存在 4.0 的问题，同时保留编译期类型安全。

保留字符串键查找的价值在于：可选子系统（profiler、jsonui debugger）可以各自挂自己的接口而不触碰核心头；插件之间也能互相暴露接口（`register_interface("myteam.foo", 1, &table)`），让一个插件给另一个插件当底座。

### 3.2 退出条件（规范）

若将来出现"第三方发布预编译插件且不跟随 mcdk 重编"的场景，应切换为逐函数查名。由于 SDK 封装挡在用户前面，届时用户代码无需改动。

## 4. 生命周期阶段

阶段划分对齐现有 `startGame()` / `launchGameExe()` 的真实流程。宿主按序调用 `on_stage`，反序调用停止逻辑。

```c
typedef uint32_t mcdk_stage;
enum {
    MCDK_STAGE_REGISTER = 0,  /* 只能注册：事件、MCP 工具、RPC、命令、配置 schema */
    MCDK_STAGE_CONFIG   = 1,  /* UserConfig 已解析，可修改 modDirs / hotReload / windowStyle */
    MCDK_STAGE_WORLD    = 2,  /* 世界目录与 pack manifest 已就绪，落盘前可干预 */
    MCDK_STAGE_RUNTIME  = 3,  /* launchGameExe 内部，ipcServer/logBuffer/mcpServer 均已就绪 */
    MCDK_STAGE_SHUTDOWN = 4,
};
```

`MCDK_STAGE_REGISTER` 的收束点与现有 `RpcRegistry::seal()` 对齐：注册窗口关闭后注册表封存，运行期只读无锁。在错误阶段调用注册类 API **必须**返回 `MCDK_ERR_WRONG_STAGE`。

阶段与宿主代码的具体触发位置见 [08-host-integration.md](08-host-integration.md) §3。阶段与事件的时序关系见 [04-events.md](04-events.md) §2。

## 5. 终结与资源回收（规范）

v1 不支持热卸载，但进程退出路径必须定义清楚：后台线程加已析构对象是插件宿主最常见的崩溃来源，而且崩在退出路径上极难排查。

### 5.1 终结顺序

宿主**必须**逆着加载顺序逐个终结插件，每个插件严格按以下六步：

```text
1. 停止向该插件派发新事件（标记其全部订阅失效）
2. 等待该插件所有 in-flight 回调返回；QUEUED 队列中属于它的待派发事件直接丢弃
3. 注销其全部事件订阅
4. 调用 on_unload(user)
5. 回收其遗留的宿主资源：图像句柄、MCP 工具注册项等
   每回收一项泄漏都打印一条带插件 id 的警告
6. 作废其 mcdk_handle self
```

**第 2 步是关键**：必须先断开派发并排空 in-flight，再调 `on_unload`。顺序反了，事件会打进正在析构的插件对象。

### 5.2 插件侧义务（规范）

- `on_unload` 返回前**必须** join 自己创建的全部线程；
- `on_unload` 返回后**禁止**再调用任何 mcdk 接口。

### 5.3 句柄作废后的兜底（规范）

`self` 作废后，宿主对任何接口调用**必须**返回 `MCDK_ERR_INVALID_HANDLE`，**禁止**崩溃。无返回值的函数（`console` 的 `log` / `log_colored`、`image_release`）静默丢弃调用。

这是针对"插件没把线程 join 干净"的兜底。不能假设插件守规矩，但**必须**保证它违规时 mcdk 报错而不是段错误——插件是第三方代码，宿主的健壮性不能建立在它的正确性上。

### 5.4 进程退出时禁止 FreeLibrary（规范）

v1 **禁止**在进程退出时主动 `FreeLibrary` / `dlclose` 插件，交给操作系统处理。

理由：插件的静态对象析构、可能残留的线程、以及两侧 CRT 的卸载顺序三者叠加，主动卸载带来的崩溃概率远高于它回收的那点资源——而进程马上就要结束了。等热卸载能力真正落地时，再单独设计这条路径并配套测试。
