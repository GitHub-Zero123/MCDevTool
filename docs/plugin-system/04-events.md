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
  ├─ mcdk.world.resolve         *  ← 可改写存档设置，见 §4.5
  ├─ mcdk.config.resolve.before    ← 可改 UserConfig
  └─ mcdk.config.resolve.finish
STAGE_WORLD
  ├─ mcdk.pack.link.before         ← 可增删改待链接 Pack
  ├─ mcdk.pack.link.finish
  ├─ mcdk.world.deploy.before
  └─ mcdk.world.deploy.finish
STAGE_RUNTIME
  ├─ mcdk.game.launch.before    *  ← v1 只能否决，见 §4.1
  ├─ mcdk.game.process.create   *  ← 可接管进程创建，见 §4.4
  ├─ mcdk.game.launch.finish    *  ← 携带 pid
  ├─ mcdk.ipc.client.connected  *  ← execute_python 自此可用
  ├─ mcdk.log.line / .error     *  ← 高频，默认 QUEUED
  ├─ （非 v1：hotreload / mcp.tool_call / host_bridge）
  ├─ mcdk.game.state_changed    *
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
    MCDK_DISPATCH_SYNC   = 1   /* 发射线程内同步调用，可否决；阻塞它就是阻塞那个子系统 */
    /* 2 曾是 MCDK_DISPATCH_MAIN，v1 发布前移除，取值永久保留不复用 */
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
} mcdk_iface_events;
```

### 3.1 事件 id 的语义（规范）

- `resolve` 返回 **0 表示该宿主不认识这个事件名**。`subscribe` 传入 0 返回无效 token（0）。插件**应该**据此优雅降级——在新宿主上多用一个事件、在旧宿主上少用一个，而不是直接判定加载失败。
- 事件 id **只在本进程本次运行内稳定**，禁止序列化、缓存到文件或硬编码。跨版本、跨进程都可能变。
- **事件不重放。** 在某事件已经发射之后才订阅，不会补发。因此依赖早期事件（`mcp.register.before` 等）的订阅**必须**在 `MCDK_STAGE_REGISTER` 内完成。
- **标「SYNC（强制）」的事件忽略订阅方要求的派发模式，一律同步。** 注册窗口类事件只能如此：宿主发完就封存注册表，异步 handler 醒来时窗口已经关了，而且关得毫无声响——注册会静默丢失。

### 3.2 设计约束

- **`mcdk.log.line` 是高频事件**（Safaia 日志管道），默认必须走 `QUEUED` + 有界队列。队列满时丢弃并计入告警计数，**禁止**让插件拖慢游戏日志读取。若插件确需逐行拦截，只能显式选择 `SYNC` 并自行保证极低开销。
- **payload 必须是版本化 POD 结构体**，不得一律用 JSON。日志事件每秒数百条，JSON 序列化开销不可接受。插件自定义的动态事件**可以**用 JSON 文本。
- 每个 handler 有执行时长监测，连续超时的插件自动降级并向用户报告。
- `priority` 小者先执行；同优先级按插件在 `.mcdev.json` 中的声明顺序（见 [06-loading.md](06-loading.md)）。

## 4. 事件清单（v1）

v1 列标注该事件是否在首版实现。事件的取舍与 [05-interfaces.md](05-interfaces.md) 的接口范围保持一致：**不为已推迟的接口提供事件**，否则插件收到事件却无从动作。

| 事件名 | v1 | 发射线程 | 可否决 | 默认派发 | 说明 |
| --- | :-: | --- | --- | --- | --- |
| `mcdk.mcp.register.before` | ✓ | 主线程 | 否 | SYNC（强制） | **MCP 工具注册窗口开启。** 插件在此调用 `mcdk.mcp` 绑定或注册工具 |
| `mcdk.mcp.register.finish` | ✓ | 主线程 | 否 | SYNC（强制） | **MCP 工具注册完成、注册表已封存。** payload 携带最终工具数，可用于自检与日志 |
| `mcdk.world.resolve` | ✓ | 主线程 | 是 | SYNC（强制） | **即将准备存档。** 插件可改写存档设置，见 §4.5 |
| `mcdk.game.launch.before` | ✓ | 主线程 | 是 | SYNC | **游戏启动前。** 否决则不启动。见 §4.1 |
| `mcdk.game.process.create` | ✓ | 主线程 | 是 | SYNC（强制） | **即将创建游戏进程。** 插件可接管创建，见 §4.4 |
| `mcdk.game.launch.finish` | ✓ | 主线程 | 否 | SYNC | **游戏进程已创建。** payload 携带 pid |
| `mcdk.game.exit` | ✓ | 进程监视线程 | 否 | QUEUED | 携带退出码 |
| `mcdk.game.state_changed` | ✓ | 启动线程 / IPC 线程 | 否 | QUEUED | **生命周期状态迁移**：加载中 / 主菜单 / 在世界里 / 已退出。判定只基于 IPC 连接，**不保证准确**，见 [05](05-interfaces.md) §5.1 |
| `mcdk.log.line` | ✓ | 日志读取线程（Safaia 模式下是启动线程） | 是 | QUEUED | `VETO` 抑制该行的控制台输出，见 §4.2 |
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

需要改命令行或环境变量的插件，现在可以用 `mcdk.game.process.create` 接管进程创建（§4.4）：payload 里有完整的命令行与环境块，插件改完自己起进程。`env_set` 仍未提供——它只在「不接管、只改一个变量」时更省事。

这是一处值得单独确认的取舍——若"启动前改环境变量"是实际用例，`env_set` 的实现成本很低（`GameEnvironmentBuilder` 已存在），可以拉进 v1。

### 4.0 为什么没有「主线程」派发（规范）

早期设计里有第三种派发模式 `MCDK_DISPATCH_MAIN`，以及配套的 `post_main`。两者在 v1 发布前被移除。

理由是它解决的问题不存在：**mcdk 没有任何线程亲和的资源**。六张接口表里，`mcdk.core` / `mcdk.console` / `mcdk.info` / `mcdk.log` / `mcdk.game` 全部可从任意线程调用，`mcdk.mcp` 的「仅主线程」其实是**阶段**限制（`McpToolRegistry::bind` 自带锁）。所谓「主线程」不过是恰好阻塞在 `WaitForSingleObject` 上的那条线程，没有特殊地位。

代价却是实在的：它要求宿主在主线程上设一个抽水点，于是游戏等待循环被迫改成可唤醒的形态；派发时还要把 in-flight 所有权从派发线程转交给主线程，`detachSubscriber` 也因此必须边等边抽水，否则它自己就跑在主线程上、会把自己锁死。

现在的模型与 mcmod 一致：**事件在干活的那条线程上广播**（`SYNC`），想异步就丢给宿主的派发线程（`QUEUED`）。每个事件的发射线程写在 §4 的表里，也标在 SDK 的 `ev::` 结构体上。

取值 `2` 永久保留不复用。将来真出现线程亲和的宿主资源（例如窗口输入注入），按 [02-abi-contract.md](02-abi-contract.md) §8 的追加规则加回来即可，不影响任何已发布的东西。

### 4.1.1 `mcdk.ipc.client.*` 不保证成对（规范）

`DebugIPCServer::stop()` 关服务时是直接清空客户端表的，**不会为它们逐个发
`disconnected`**。那个时点插件多半已经终结，事件总线也停了，通知没有去处。

因此插件**禁止**把「每一次 connected 都会收到配对的 disconnected」当作前提。要收摊就看
`mcdk.game.exit` 或 `onShutdown`，那两个才是终止信号。

### 4.2 `mcdk.log.line` 的 VETO 语义（规范）

**`VETO` 只抑制该行的控制台输出，不影响 `LogBuffer`。** 被否决的行仍然进入日志缓冲区，`mcdk.log` 接口与 MCP 的 `get_latest_logs` 仍能读到它。

### 4.3 否决与异步订阅者（规范）

否决只能来自 `SYNC` 订阅者。一旦被否决，该事件**是否还会投递给 `QUEUED` / `MAIN` 订阅者，取决于否决到底取消了什么**：

| 事件 | 否决取消的是 | 异步订阅者是否仍收到 |
| --- | --- | :-: |
| `mcdk.game.launch.before` | 整个操作（游戏不会启动） | 否 |
| `mcdk.log.line` / `.error` | 仅控制台输出（日志行仍存在） | 是 |

判据是「这件事还算发生过吗」。`.before` 被否决后它没发生，再投过去只会让异步订阅者枯等一个永远不来的 `.finish`；而日志行被抑制输出后它依然存在，异步订阅者照收。

该语义逐事件登记在 `event_bus.cpp` 的 `kTraits` 表中（`vetoCancelsEvent`），新增可否决事件时必须同步填写。

理由有二。其一，`LogBuffer` 是诊断真源：一个用于过滤噪音的插件若同时让 AI 在排查时看不到那些行，会造成极难定位的"日志莫名缺失"。其二，这个选择是**可逆**的——将来若确有需求，可以追加一个 `MCDK_EVENT_VETO_ALL` 取值来同时过滤缓冲区；反过来，一旦放行了"插件能从缓冲区抹掉日志"，再收回就是破坏性变更。

**v1 不提供改写日志文本的能力。** 事件 payload 是 `const`，没有回写通道。要提供改写就得引入可变 payload，这会给整个事件系统增加一类需要单独定义所有权与并发语义的机制，收益不匹配。需要变形后的日志，插件自己输出一份到 `mcdk.console` 即可。

### 4.4 `mcdk.game.process.create`：接管进程创建（规范）

发射点就是宿主调用 `CreateProcessW` 的位置。此时 IPC 服务已在监听、MCP 已启动、日志管道已建好，
payload 里的命令行、环境块、三个 std 句柄就是宿主本来要传给 `CreateProcessW` 的东西。

**为什么不复用 `launch.before`。** 它刻意放在所有子系统搭起来之前，否决时什么都不用拆；
那个时刻 IPC 端口、管道、环境变量都还不存在，交给插件也无从使用。

**为什么是「插件交回进程」而不是「否决后插件自己起」。** 启动之后 mcdk 离不开这个进程：
pid 喂给 profiler、热更新、UI 重载、样式处理器、MCP；句柄用来等待退出、取退出码；管道是非 Safaia
模式下日志的唯一来源；环境变量里有调试 Mod 要读的 IPC 端口。插件只否决不交回，mcdk 就成了空壳。

#### 契约

插件接管时**必须**：

1. 以 `CREATE_SUSPENDED` 创建进程，且只挂起一次；
2. `bInheritHandles = TRUE`，并以 payload 的 `std_input` / `std_output` / `std_error` 作为子进程的 std 句柄；
3. 交回的 pid 是**游戏进程本身**，不是某个启动器或包装进程；
4. 调用 `mcdk.game.commit_process(request, pid, tid)`，然后返回 `STOP`；
5. 关闭自己的 `hProcess` / `hThread`——宿主会另开。

环境块可以改，但**应该**保留宿主写入的 `MCDEV_*` 变量，否则调试 Mod 连不回来。
SDK 的 `mcdk/plugin/process.hpp` 把 1、2、4、5 收成了 `mcdk::process::launch()`，见 [07-sdk.md](07-sdk.md) §7。

#### 宿主的处理

| 插件的行为 | 宿主的反应 |
| --- | --- |
| 返回 `CONTINUE`，未 commit | 交给下一个 handler；都没接管则走默认的 `CreateProcessW` |
| commit | 立即停止派发（不论返回值），接手该进程 |
| 返回 `VETO`，未 commit | 启动失败，报错并点名该插件，中止启动 |
| 返回 `STOP`，未 commit | 视为插件 bug，同上 |
| 第二次 commit | 返回 `MCDK_ERR_DUPLICATE`。实际上到不了：第一次 commit 后派发就停了 |

接手时宿主用 pid 与 tid 自己 `OpenProcess` / `OpenThread`，并校验 tid 属于 pid（不符则 commit 返回
`MCDK_ERR_INVALID_ARGUMENT`）。之后与默认路径汇合：Safaia 模式先启动日志接收器，然后 `ResumeThread`。
**`ResumeThread` 返回的旧挂起计数必须恰好是 1**：0 说明进程早已在跑，大于 1 说明恢复后仍挂着。
两种都终止该进程、报错点名、中止启动。

「commit 即停止派发」而不是「看返回值」，是因为 SDK 的异常屏障会把抛异常的 handler 记成
`CONTINUE`。若插件起了进程、commit 成功、随后抛了异常，按返回值走就会让下一个插件再起一个。

宿主在交接途中失败（例如 Safaia 接收器起不来）时，受理窗口析构会终止那个仍挂起的进程，不留孤儿。

#### 必须挂起的理由

- **pid 不会失效。** 挂起的进程不会退出，宿主拿 pid 去 `OpenProcess` 一定打开的是同一个进程，不存在「已退出、pid 被复用」的竞态。
- **不丢日志。** Safaia 接收器必须在游戏跑起来之前就绪，由宿主掌握恢复时机才保证得了。
- **孙进程能被识破。** 插件若交回某个启动器的 pid，那个启动器多半不是挂起的，挂起计数的校验会拦住它，而不是让 mcdk 静默盯着一个错的进程。

通过 RenderDoc、PIX 等工具间接拉起游戏（交回的不是挂起的直接子进程）不在本契约之内。

#### 为什么结果走接口函数而不是可变 payload

payload 保持 `const`，结果经宿主句柄 `request` 加 `commit_process` 交回，与 `bind_tool`、图像句柄同一套做法。
§4.2 拒绝可变 payload 的理由在这里同样成立：它会让整个事件系统多出一类要单独定义所有权与并发语义的机制。

`request` 只在本次派发内有效，派发结束后 commit 一律返回 `MCDK_ERR_INVALID_HANDLE`。
`commit_process` 不受阶段矩阵约束（发射时 RUNTIME 尚未到来），它的有效期就是这个窗口。

### 4.5 `mcdk.world.resolve`：改写存档设置（规范）

典型用途：定制版编辑器（经 `MCDEV_PLUGINS` 注入插件，见 [06-loading.md](06-loading.md) §2.4）把游戏
引到编辑器专用的存档，而不改用户 `.mcdev.json` 里的那个。

发射点在 `startGame()` 里 CONFIG 阶段刚开始、处理存档之前：删旧存档、部署地图、写 `level.dat`、
写自动进入存档的配置都在它之后，用的都是改写后的设置；插件读到的 `mcdk.info` 与 Host Bridge 的会话
信息也一样。子进程模式（`MCDEV_IS_SUBPROCESS_MODE`）不处理存档，不发此事件。

payload 的 `world_json` 是当前设置，插件调 `mcdk.game.override_world(request, settings_json, mode)` 改写：

- `settings_json` 是 JSON 对象，**键名与 `.mcdev.json` 完全相同**：`world_name`、`world_folder_name`、
  `world_source_path`、`reset_world`、`auto_join_game`、`world_seed`、`world_type`、`game_mode`、
  `enable_cheats`、`keep_inventory`、`do_weather_cycle`、`do_daylight_cycle`、`experiment_options`。
- `MCDK_WORLD_OVERRIDE_MERGE`（按键覆盖）：只改给出的键，其余沿用用户的设置。
- `MCDK_WORLD_OVERRIDE_REPLACE`（完全覆盖）：给出的键之外一律回到默认值，用户的存档设置一概不继承。

两种模式解析时用的是同一份代码（`applyWorldConfig`），`.mcdev.json` 以后新增的存档键插件自动可用。

#### 校验

以下情况 `override_world` 返回 `MCDK_ERR_INVALID_ARGUMENT`，原因可用 `get_last_error`（SDK 的
`ctx.lastHostError()`）取到，宿主不做任何改动：

- 不是 JSON 对象，或含有上面列表之外的键（防 `reset_wrold` 这类拼写错误被静默忽略）；
- 值的类型不对；
- `world_source_path` 指向的目录不含 `level.dat`；
- **`world_folder_name` 不是单层目录名**：空、`.`、`..`、含 `/ \ : * ? " < > |` 或控制字符、以点或空格结尾。
  `reset_world` 会整个删掉这个目录，放过 `..` 就等于允许插件删任意目录。

#### 宿主的处理

与 `mcdk.game.process.create`（§4.4）同一套规则：**第一个改写成功的插件生效，派发随即停止**，之后再改写
返回 `MCDK_ERR_DUPLICATE`。两个插件都想决定进哪个存档本身就是冲突，「各改各的键」会让谁覆盖谁说不清。

| 插件的行为 | 宿主的反应 |
| --- | --- |
| 改写成功 | 立即停止派发，按改写后的设置继续，并打印「插件 X 改写了存档设置（按键覆盖 / 完全覆盖）：目录名」 |
| 返回 `CONTINUE`，未改写 | 交给下一个 handler；都没改写则沿用原设置 |
| 返回 `STOP`，未改写 | 沿用原设置，后面的插件不再收到 |
| 返回 `VETO`，未改写 | 启动中止并点名该插件。适用于「进不了编辑器存档就别启动」 |

`settings_json` 走 JSON 而不是逐字段的 C 结构体：只在启动时调用一次，没有性能顾虑；「只改一部分」
是 JSON 天然的表达；存档键还会继续增加，结构体每加一个字段都是一次 ABI 追加。

## 5. payload 定义

每个事件 payload 是独立 POD 结构体，同样遵循 `struct_size` 追加规则（见 [02-abi-contract.md](02-abi-contract.md) §5.8）。

```c
/* abi/events.h */

typedef struct mcdk_ev_mcp_register {
    uint32_t struct_size;
    uint32_t tool_count;       /* before: 已有内置工具数；finish: 最终工具总数 */
} mcdk_ev_mcp_register;

typedef struct mcdk_ev_world_resolve {
    uint32_t    struct_size;
    uint32_t    _reserved;
    mcdk_handle request;          /* override_world 的凭据，仅本次派发内有效 */
    mcdk_str    world_json;       /* 借用；当前存档设置，键名与 .mcdev.json 相同 */
} mcdk_ev_world_resolve;

typedef struct mcdk_ev_game_launch_before {
    uint32_t struct_size;
    uint32_t _reserved;
    mcdk_str exe_path;            /* 借用 */
    mcdk_str dev_config_path;     /* 借用，未启用自动进入存档时为空 */
    /* 后续版本在此追加 env_builder 句柄，见 §4.1 */
} mcdk_ev_game_launch_before;

typedef struct mcdk_ev_game_process_create {
    uint32_t    struct_size;
    uint32_t    _reserved;
    mcdk_handle request;          /* commit_process 的凭据，仅本次派发内有效 */
    mcdk_str    exe_path;         /* 借用 */
    mcdk_str    command_line;     /* 借用；完整命令行，首段是带引号的 exe */
    mcdk_str    environment;      /* 借用；"K=V\0K=V\0\0"，len 含结尾的两个 \0 */
    uint64_t    std_input;        /* 可继承的 HANDLE */
    uint64_t    std_output;
    uint64_t    std_error;
} mcdk_ev_game_process_create;

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

§4 中标为 v1 的十二个事件共用九个 payload 结构体：以上八个，外加 `game.state_changed` 的 `mcdk_ev_game_state`（`mcp.register.before/finish` 共用 `mcdk_ev_mcp_register`，`log.line/error` 共用 `mcdk_ev_log_line`，`ipc.client.connected/disconnected` 共用 `mcdk_ev_ipc_client`）。全部登记在 [13-registry.md](13-registry.md)。

上表中标注为非 v1 的事件，其 payload 结构体在 v1 阶段**不定义**——按 [02-abi-contract.md](02-abi-contract.md) §5.9，一旦定义就只能追加不能改，过早固化没有实现依据的布局是最容易留下历史包袱的做法。

将来为 `mcdk_ev_game_launch_before` 补 `env_builder` 时，它是对现有 `GameEnvironmentBuilder` 的句柄化封装，并且将是"启动前改环境变量"的唯一入口——插件**禁止**直接调用平台 API 修改子进程环境。

## 6. SDK 侧用法

用户不接触 `event_id` 与 payload 结构体：

```cpp
void onRegister(mcdk::Context& ctx) override {
    // MCP 工具注册窗口
    ctx.events().on<mcdk::ev::McpRegisterBefore>([&](const auto&) {
        // 描述符写在 plugin.json 的 mcpTools 里，这里只补 handler。
        ctx.mcp().bindTool("my_tool", handler);
    });

    ctx.events().on<mcdk::ev::McpRegisterFinish>([&](const auto& e) {
        ctx.console().info("MCP 工具共 " + std::to_string(e.toolCount) + " 个");
    });

    // 游戏启动前后
    ctx.events().on<mcdk::ev::GameLaunchBefore>([&](const auto& e) {
        return readyToLaunch(e.exePath) ? mcdk::EventResult::Continue
                                        : mcdk::EventResult::Veto;
    });

    // 接管进程创建（需 #include <mcdk/plugin/process.hpp>）
    ctx.events().on<mcdk::ev::GameProcessCreate>([&](const auto& e) {
        auto spec = mcdk::process::LaunchSpec::from(e);
        spec.commandLine += L" --my-flag";
        return mcdk::process::launch(ctx.game(), e, std::move(spec));
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
