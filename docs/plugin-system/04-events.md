# 04 · 事件系统

上级索引：[README.md](README.md)　前置阅读：[02-abi-contract.md](02-abi-contract.md)、[03-abi-reference.md](03-abi-reference.md)

Godot 没有可借鉴的对应物（它用对象系统内的 signal，GDExtension 层没有独立事件设施），本章为自行设计。

## 1. 命名约定（规范）

1. 事件名统一为 `mcdk.<domain>.<action>[.before|.finish]`，全小写，点分。
2. **凡是插件既需要"事前干预"又需要"事后知晓"的操作，必须成对提供 `.before` 与 `.finish`。** 只有单点通知语义的事件不加后缀。
3. `.before` 事件**必须**是可否决的，且**必须**以 `MCDK_DISPATCH_SYNC` 派发——否则无法干预。
4. `.finish` 事件**禁止**可否决：操作已经发生，否决没有意义。
5. 插件自定义事件**必须**使用自己的反向域名前缀（`com.example.foo.bar`），**禁止**占用 `mcdk.` 命名空间。

## 2. 阶段与事件的时序

事件在生命周期阶段（见 [03-abi-reference.md](03-abi-reference.md) §4）之间发射：

`*` 标记的为 v1 实现范围，其余见 §4 的 v1 列。

```text
STAGE_REGISTER
  ├─ mcdk.mcp.register.before   *  ← 插件在此注册 MCP 工具
  ├─ mcdk.mcp.register.finish   *  ← 注册表封存，工具清单已定
  ├─ mcdk.rpc.register.before
  └─ mcdk.rpc.register.finish
STAGE_CONFIG
  ├─ mcdk.config.resolve.before    ← 可改 UserConfig
  └─ mcdk.config.resolve.finish
STAGE_WORLD
  ├─ mcdk.pack.link.before         ← 可增删改待链接 Pack
  ├─ mcdk.pack.link.finish
  ├─ mcdk.world.deploy.before
  └─ mcdk.world.deploy.finish
STAGE_RUNTIME
  ├─ mcdk.game.launch.before    *  ← v1 只能否决，见 §4.1
  ├─ mcdk.game.launch.finish    *  ← 携带 pid
  ├─ mcdk.ipc.client.connected  *  ← execute_python 自此可用
  ├─ mcdk.log.line / .error     *  ← 高频，默认 QUEUED
  ├─ （非 v1：hotreload / mcp.tool_call / host_bridge）
  └─ mcdk.game.exit             *
STAGE_SHUTDOWN
```

> 实现状态：总线位于 `tools/mcdk/src/plugin_host/event_bus.cpp`，热路径入口
> `tools/mcdk/include/mcdk/plugin_host/events.hpp`，ABI 在
> `sdk/plugin-sdk/include/mcdk/plugin/abi/events.h` 与 `abi/iface/events.h`，
> 插件侧的类型化封装在 `sdk/plugin-sdk/include/mcdk/plugin/events.hpp`。
> 各事件的接入进度见 [13-registry.md](13-registry.md) §4.1。

## 3. 线程与派发模式

宿主当前存在主线程、5 个热更新 watcher 线程、MCP server 线程、IPC 线程、Host Bridge 线程。**规范：事件回调所在线程必须是 ABI 的一部分，由订阅方在订阅时声明。**

```c
typedef uint32_t mcdk_dispatch_mode;
enum {
    MCDK_DISPATCH_QUEUED = 0,  /* 默认。payload 深拷贝，插件专用线程串行回调 */
    MCDK_DISPATCH_SYNC   = 1,  /* 发射线程内同步调用，可改 payload / 否决，必须极快 */
    MCDK_DISPATCH_MAIN   = 2,  /* 投递到主线程 */
};

typedef uint32_t mcdk_event_result;
enum {
    MCDK_EVENT_CONTINUE = 0,
    MCDK_EVENT_STOP     = 1,   /* 停止后续 handler */
    MCDK_EVENT_VETO     = 2,   /* 仅对可否决事件有效，其余事件忽略 */
};

typedef mcdk_event_result (MCDK_CALL *mcdk_event_handler)(const mcdk_event* ev, void* user);

typedef struct mcdk_event {
    uint32_t    struct_size;
    uint32_t    event_id;
    uint32_t    payload_version;
    uint32_t    payload_size;
    const void* payload;       /* 借用，回调返回即失效 */
} mcdk_event;

typedef struct mcdk_iface_events {
    uint32_t struct_size;
    uint32_t    MCDK_CALL (*resolve)(mcdk_str name);      /* 字符串 → 数值 id，热路径只比整数 */
    mcdk_handle MCDK_CALL (*subscribe)(mcdk_handle self, uint32_t event_id,
                                       mcdk_dispatch_mode mode, int32_t priority,
                                       mcdk_event_handler handler, void* user);
    void        MCDK_CALL (*unsubscribe)(mcdk_handle self, mcdk_handle token);
    mcdk_status MCDK_CALL (*emit)(mcdk_handle self, uint32_t event_id,
                                  const void* payload, uint32_t payload_size);
    void        MCDK_CALL (*post_main)(mcdk_handle self,
                                       void (MCDK_CALL *fn)(void*), void* user);
} mcdk_iface_events;
```

### 3.1 事件 id 的语义（规范）

- `resolve` 返回 **0 表示该宿主不认识这个事件名**。`subscribe` 传入 0 返回无效 token（0）。插件**应该**据此优雅降级——在新宿主上多用一个事件、在旧宿主上少用一个，而不是直接判定加载失败。
- 事件 id **只在本进程本次运行内稳定**，禁止序列化、缓存到文件或硬编码。跨版本、跨进程都可能变。
- **事件不重放。** 在某事件已经发射之后才订阅，不会补发。因此依赖早期事件（`mcp.register.before` 等）的订阅**必须**在 `MCDK_STAGE_REGISTER` 内完成。

### 3.2 设计约束

- **`mcdk.log.line` 是高频事件**（Safaia 日志管道），默认必须走 `QUEUED` + 有界队列。队列满时丢弃并计入告警计数，**禁止**让插件拖慢游戏日志读取。若插件确需逐行拦截，只能显式选择 `SYNC` 并自行保证极低开销。
- **payload 必须是版本化 POD 结构体**，不得一律用 JSON。日志事件每秒数百条，JSON 序列化开销不可接受。插件自定义的动态事件**可以**用 JSON 文本。
- 每个 handler 有执行时长监测，连续超时的插件自动降级并向用户报告。
- `priority` 小者先执行；同优先级按插件在 `.mcdev.json` 中的声明顺序（见 [06-loading.md](06-loading.md)）。

## 4. 事件清单（v1）

v1 列标注该事件是否在首版实现。事件的取舍与 [05-interfaces.md](05-interfaces.md) 的接口范围保持一致：**不为已推迟的接口提供事件**，否则插件收到事件却无从动作。

| 事件名 | v1 | 发射线程 | 可否决 | 默认派发 | 说明 |
| --- | :-: | --- | --- | --- | --- |
| `mcdk.mcp.register.before` | ✓ | 主线程 | 否 | SYNC | **MCP 工具注册窗口开启。** 插件在此调用 `mcdk.mcp` 注册自己的工具 |
| `mcdk.mcp.register.finish` | ✓ | 主线程 | 否 | SYNC | **MCP 工具注册完成、注册表已封存。** payload 携带最终工具数，可用于自检与日志 |
| `mcdk.game.launch.before` | ✓ | 主线程 | 是 | SYNC | **游戏启动前。** 否决则不启动。见 §4.1 |
| `mcdk.game.launch.finish` | ✓ | 主线程 | 否 | SYNC | **游戏进程已创建。** payload 携带 pid |
| `mcdk.game.exit` | ✓ | 进程监视线程 | 否 | QUEUED | 携带退出码 |
| `mcdk.log.line` | ✓ | 日志读取线程 | 是 | QUEUED | `VETO` 抑制该行的控制台输出，见 §4.2 |
| `mcdk.log.error` | ✓ | 日志读取线程 | 是 | QUEUED | 同上，stderr 通道 |
| `mcdk.ipc.client.connected` | ✓ | IPC 线程 | 否 | QUEUED | 实际的"已进入世界"信号，`execute_python` 自此可用 |
| `mcdk.ipc.client.disconnected` | ✓ | IPC 线程 | 否 | QUEUED | — |
| `mcdk.rpc.register.before` | — | 主线程 | 否 | SYNC | 待 `mcdk.rpc` 开放 |
| `mcdk.rpc.register.finish` | — | 主线程 | 否 | SYNC | 同上 |
| `mcdk.config.resolve.before` | — | 主线程 | 是 | SYNC | 待 `mcdk.config` 开放 |
| `mcdk.config.resolve.finish` | — | 主线程 | 否 | SYNC | 同上 |
| `mcdk.pack.link.before` | — | 主线程 | 是 | SYNC | 待 `mcdk.pack` 开放 |
| `mcdk.pack.link.finish` | — | 主线程 | 否 | SYNC | 同上 |
| `mcdk.world.deploy.before` | — | 主线程 | 是 | SYNC | 同上 |
| `mcdk.world.deploy.finish` | — | 主线程 | 否 | SYNC | 同上 |
| `mcdk.hotreload.file_changed` | — | watcher 线程 | 是 | QUEUED | 待 `mcdk.hotreload` 开放 |
| `mcdk.hotreload.trigger.before` | — | watcher 线程 | 是 | SYNC | 同上 |
| `mcdk.hotreload.trigger.finish` | — | watcher 线程 | 否 | QUEUED | 同上 |
| `mcdk.mcp.tool_call.before` | — | MCP 工作线程 | 是 | SYNC | `VETO` 拒绝该次调用，用于访问控制 |
| `mcdk.mcp.tool_call.finish` | — | MCP 工作线程 | 否 | QUEUED | 审计用 |
| `mcdk.host_bridge.connected` | — | Host Bridge 线程 | 否 | QUEUED | — |

v1 九条事件构成一个自洽的闭环：注册期拿到 MCP 开口，启动期知道游戏起没起，运行期知道能不能执行代码、能拿到日志流。

### 4.1 `mcdk.game.launch.before` 在 v1 的能力边界

**v1 的该事件只能否决启动，不能修改子进程环境变量或命令行。**

原有设计是在 payload 中带一个 `env_builder` 句柄，指向现有的 `GameEnvironmentBuilder`。但那需要 `mcdk.game` 暴露 `env_set`，超出 [05-interfaces.md](05-interfaces.md) §1 划定的 v1 范围，故一并推迟。

在此之前，插件在该事件里能做的是：条件性否决启动、启动前往 `project_root` 写文件、拉起辅助进程。

这是一处值得单独确认的取舍——若"启动前改环境变量"是实际用例，`env_set` 的实现成本很低（`GameEnvironmentBuilder` 已存在），可以拉进 v1。

### 4.2 `mcdk.log.line` 的 VETO 语义（规范）

**`VETO` 只抑制该行的控制台输出，不影响 `LogBuffer`。** 被否决的行仍然进入日志缓冲区，`mcdk.log` 接口与 MCP 的 `get_latest_logs` 仍能读到它。

理由有二。其一，`LogBuffer` 是诊断真源：一个用于过滤噪音的插件若同时让 AI 在排查时看不到那些行，会造成极难定位的"日志莫名缺失"。其二，这个选择是**可逆**的——将来若确有需求，可以追加一个 `MCDK_EVENT_VETO_ALL` 取值来同时过滤缓冲区；反过来，一旦放行了"插件能从缓冲区抹掉日志"，再收回就是破坏性变更。

**v1 不提供改写日志文本的能力。** 事件 payload 是 `const`，没有回写通道。要提供改写就得引入可变 payload，这会给整个事件系统增加一类需要单独定义所有权与并发语义的机制，收益不匹配。需要变形后的日志，插件自己输出一份到 `mcdk.console` 即可。

## 5. payload 定义

每个事件 payload 是独立 POD 结构体，同样遵循 `struct_size` 追加规则（见 [02-abi-contract.md](02-abi-contract.md) §5.8）。

```c
/* abi/events.h */

typedef struct mcdk_ev_mcp_register {
    uint32_t struct_size;
    uint32_t tool_count;       /* before: 已有内置工具数；finish: 最终工具总数 */
} mcdk_ev_mcp_register;

typedef struct mcdk_ev_game_launch_before {
    uint32_t struct_size;
    uint32_t _reserved;
    mcdk_str exe_path;            /* 借用 */
    mcdk_str dev_config_path;     /* 借用，未启用自动进入存档时为空 */
    /* 后续版本在此追加 env_builder 句柄，见 §4.1 */
} mcdk_ev_game_launch_before;

typedef struct mcdk_ev_game_launch_finish {
    uint32_t struct_size;
    uint32_t pid;
    mcdk_str exe_path;            /* 借用 */
} mcdk_ev_game_launch_finish;

typedef struct mcdk_ev_game_exit {
    uint32_t struct_size;
    uint32_t pid;
    int32_t  exit_code;
    uint32_t _reserved;
} mcdk_ev_game_exit;

typedef struct mcdk_ev_log_line {
    uint32_t struct_size;
    uint32_t channel;             /* mcdk_log_channel */
    int64_t  timestamp_ms;
    mcdk_str text;                /* 借用，回调返回即失效 */
} mcdk_ev_log_line;

/* connected 与 disconnected 共用 */
typedef struct mcdk_ev_ipc_client {
    uint32_t struct_size;
    uint32_t client_count;        /* 本次变化后的调试 IPC 客户端数 */
} mcdk_ev_ipc_client;
```

以上六个结构体覆盖 §4 中标为 v1 的全部九个事件（`mcp.register.before/finish` 共用 `mcdk_ev_mcp_register`，`log.line/error` 共用 `mcdk_ev_log_line`，`ipc.client.connected/disconnected` 共用 `mcdk_ev_ipc_client`）。全部登记在 [13-registry.md](13-registry.md)。

上表中标注为非 v1 的事件，其 payload 结构体在 v1 阶段**不定义**——按 [02-abi-contract.md](02-abi-contract.md) §5.9，一旦定义就只能追加不能改，过早固化没有实现依据的布局是最容易留下历史包袱的做法。

将来为 `mcdk_ev_game_launch_before` 补 `env_builder` 时，它是对现有 `GameEnvironmentBuilder` 的句柄化封装，并且将是"启动前改环境变量"的唯一入口——插件**禁止**直接调用平台 API 修改子进程环境。

## 6. SDK 侧用法

用户不接触 `event_id` 与 payload 结构体：

```cpp
void onRegister(mcdk::Context& ctx) override {
    // MCP 工具注册窗口
    ctx.events().on<mcdk::ev::McpRegisterBefore>([&](const auto&) {
        ctx.mcp().addTool("my_tool", "说明", schema, handler);
    });

    ctx.events().on<mcdk::ev::McpRegisterFinish>([&](const auto& e) {
        ctx.console().info("MCP 工具共 " + std::to_string(e.toolCount) + " 个");
    });

    // 游戏启动前后
    ctx.events().on<mcdk::ev::GameLaunchBefore>([&](const auto& e) {
        return readyToLaunch(e.exePath) ? mcdk::EventResult::Continue
                                        : mcdk::EventResult::Veto;
    });

    ctx.events().on<mcdk::ev::GameLaunchFinish>([&](const auto& e) {
        ctx.console().info("游戏 pid = " + std::to_string(e.pid));
    });

    // 游戏进入世界后 execute_python 才可用
    ctx.events().on<mcdk::ev::IpcClientConnected>([&](const auto&) {
        ctx.game().executePython("print('hello from plugin')", mcdk::Side::Server);
    });
}
```
