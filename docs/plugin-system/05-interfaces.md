# 05 · v1 接口定义

上级索引：[README.md](README.md)　前置阅读：[02-abi-contract.md](02-abi-contract.md)、[03-abi-reference.md](03-abi-reference.md)

本文定义 v1 的完整接口集。所有定义都必须服从 [02-abi-contract.md](02-abi-contract.md)，未在本文出现的能力一律不在 v1 范围内（见 §9）。

## 1. v1 范围

六项业务能力：

| 能力 | 接口 |
| --- | --- |
| 注册 MCP 工具 | `mcdk.mcp/1` |
| 线程安全的彩色控制台输出 | `mcdk.console/1` |
| Python 代码执行 | `mcdk.game/1` |
| 获取会话信息（游戏路径、MCP 端口、游戏 IPC 端口等） | `mcdk.info/1` |
| 屏幕捕获 | `mcdk.game/1` |
| 读取游戏日志缓冲区 | `mcdk.log/1` |

两项基础设施，业务能力依赖它们：

| 接口 | 用途 |
| --- | --- |
| `mcdk.core/1` | 错误查询、宿主版本、当前阶段 |
| `mcdk.events/1` | 事件订阅，定义见 [04-events.md](04-events.md) |

合计 6 张接口表。

## 2. 通用约定（规范）

1. 每个函数首参数都是 `mcdk_handle self`（来自 `mcdk_host_info::self`）。传入非法句柄返回 `MCDK_ERR_INVALID_HANDLE`。
2. 返回 `mcdk_status` 的函数，非 `MCDK_OK` 时**必须**同时在错误槽写入可读信息，调用方用 `mcdk.core` 的 `get_last_error` 取出。
3. 所有 `mcdk_str` 出参都是**借用**语义：仅在本次调用返回后至下一次同线程 ABI 调用之间有效，调用方必须立即拷贝。SDK 已在 C++ 层转成 `std::string`，插件作者无感。
4. 所有传入宿主的回调（`mcdk_log_sink`、`mcdk_mcp_tool_handler`）都由 SDK 套上异常屏障，**禁止**抛出异常穿越边界。
5. 带 `struct_size` 的入参结构体，调用方**必须**先清零再填 `struct_size = sizeof(...)`。

## 3. `mcdk.core/1`

```c
typedef struct mcdk_iface_core {
    uint32_t struct_size;

    /* 取出本插件线程局部错误槽中的消息。借用，立即拷贝。无错误时 len == 0 */
    void MCDK_CALL (*get_last_error)(mcdk_handle self, mcdk_str* out_message);

    /* 宿主版本串，例如 "1.4.2"。借用 */
    void MCDK_CALL (*get_host_version)(mcdk_handle self, mcdk_str* out_version);

    /* 当前生命周期阶段，取值见 mcdk_stage */
    mcdk_stage MCDK_CALL (*get_stage)(mcdk_handle self);

    /*
     * 取回 .mcdev.json 中该条插件声明的 config 字段，UTF-8 JSON 文本。借用。
     * 这是「可传参式插件」的入口：同一个二进制可以声明多次、各带不同 config，
     * 据此表现出不同行为。
     * 未设置时回填字面量 "null" 而非 len == 0，使插件永远可以直接 parse。
     * 这是本 ABI 中唯一不用 len == 0 表示「未设置」的地方。
     */
    void MCDK_CALL (*get_config)(mcdk_handle self, mcdk_str* out_config_json);
} mcdk_iface_core;
```

**错误槽是线程局部的（规范）：`get_last_error` 只返回**当前线程**上最近一次失败的信息。** 在工作线程上失败、到主线程上取，取到的是空串或另一条无关的错误。跨线程传递错误由插件自己负责。

同理，插件在 `MCDK_DISPATCH_QUEUED` 回调里调用接口失败时，错误槽位于事件派发线程，必须就地取出。

## 4. `mcdk.console/1`

```c
typedef uint32_t mcdk_log_level;
enum { MCDK_LOG_TRACE = 0, MCDK_LOG_DEBUG = 1, MCDK_LOG_INFO = 2,
       MCDK_LOG_WARN  = 3, MCDK_LOG_ERROR = 4 };

typedef uint32_t mcdk_color;
enum {
    MCDK_COLOR_DEFAULT   = 0,
    MCDK_COLOR_GREEN     = 1,
    MCDK_COLOR_RED       = 2,
    MCDK_COLOR_BLUE      = 3,
    MCDK_COLOR_YELLOW    = 4,
    MCDK_COLOR_CYAN      = 5,
    MCDK_COLOR_MAGENTA   = 6,
    MCDK_COLOR_WHITE     = 7,
    MCDK_COLOR_BLACK     = 8,
    MCDK_COLOR_GRAY      = 9,
    MCDK_COLOR_DARK_GRAY = 10,
};

typedef struct mcdk_iface_console {
    uint32_t struct_size;
    void MCDK_CALL (*log)(mcdk_handle self, mcdk_log_level level, mcdk_str message);
    void MCDK_CALL (*log_colored)(mcdk_handle self, mcdk_color color, mcdk_str message);
} mcdk_iface_console;
```

### 4.1 线程安全（规范）

**这两个函数必须可从任意线程调用，宿主负责串行化。** 实现直接复用现有的 `printColoredAtomic`。

**宿主必须保证单次调用的消息整体原子写出**，不与其他线程的输出交错。`message` 允许含换行，此时整个多行块作为一个原子单位输出；插件**禁止**通过多次调用拼接一行，那样会被其他线程撕裂。

消息末尾**不需要**自带换行，由宿主补。

两个函数都不返回状态：输出失败没有插件可采取的补救动作，且日志调用出现在错误处理路径上，返回值只会诱导出无意义的嵌套错误处理。

### 4.2 枚举值与宿主内部枚举的关系（规范）

`mcdk_color` 的取值顺序刻意与现有 `mcdk::ConsoleColor` 一致，但二者是**两个独立的枚举**。ABI 枚举值按 [02-abi-contract.md](02-abi-contract.md) §5.10 永久冻结，宿主内部枚举则可以自由调整。

因此 shim **禁止**写成 `static_cast<ConsoleColor>(color)`，必须使用显式的 `switch` 映射。否则将来有人往 `ConsoleColor` 中间插一个值，所有已编译插件的颜色会静默错位。

## 5. `mcdk.info/1`

```c
typedef struct mcdk_session_info {
    uint32_t  struct_size;
    uint32_t  mcdk_pid;
    uint32_t  game_pid;           /* 0 = 游戏进程尚未创建 */
    uint16_t  game_ipc_port;      /* 0 = 调试 IPC 未启用 */
    uint16_t  mcp_port;           /* 0 = MCP 未启用 */
    mcdk_bool mcp_enabled;
    mcdk_bool game_debug_ready;   /* 调试 IPC 已有客户端，即游戏已进入世界 */
    uint8_t   _reserved[6];
    mcdk_str  mcp_ip;
    mcdk_str  game_exe_path;
    mcdk_str  project_root;
    mcdk_str  world_name;
    mcdk_str  world_folder_name;
    mcdk_str  world_runtime_path;
    mcdk_str  world_source_path;  /* len == 0 表示非玩法地图工程 */
} mcdk_session_info;

typedef struct mcdk_iface_info {
    uint32_t struct_size;
    mcdk_status MCDK_CALL (*get_session)(mcdk_handle self, mcdk_session_info* out_info);
} mcdk_iface_info;
```

字段取值对齐现有的 `HostBridgeSessionInfo`，路径一律为 UTF-8 generic 形式（`Utils::pathToGenericUtf8`）。

**规范：标量字段是调用时刻的快照；字符串字段同样是借用，`get_session` 返回后即失效，必须立即拷贝。** 这里不对 [02-abi-contract.md](02-abi-contract.md) §6 的借用规则开任何例外——SDK 的 `ctx.info().session()` 返回内含 `std::string` 的 C++ 结构体，用户不受影响。

`game_pid` 与 `game_debug_ready` 会随时间变化，插件**禁止**缓存后长期使用，需要跟踪状态变化请订阅 `mcdk.game.launch.finish` 与 `mcdk.ipc.client.connected`。

插件私有 `config` 的读取接口在 `mcdk.core` 上（§3 的 `get_config`），不在这里。

## 6. `mcdk.game/1`

```c
typedef uint32_t mcdk_side;
enum { MCDK_SIDE_SERVER = 0, MCDK_SIDE_CLIENT = 1 };

typedef uint32_t mcdk_image_format;
enum { MCDK_IMAGE_JPEG = 0 };

typedef struct mcdk_capture_options {
    uint32_t struct_size;
    uint32_t max_height;    /* 等比缩放高度上限；0 = 宿主默认（480） */
    /* 客户区内的截取范围，归一化到 0.0~1.0，与 mc_input 同构。
       四个值全为 0 表示整块客户区。 */
    double region_left;
    double region_top;
    double region_right;
    double region_bottom;
} mcdk_capture_options;

typedef struct mcdk_image_info {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    uint32_t format;        /* mcdk_image_format */
    size_t   byte_size;
} mcdk_image_info;

typedef struct mcdk_iface_game {
    uint32_t struct_size;

    /* 阻塞至游戏返回或超时。out_result_json 借用，必须立即拷贝 */
    mcdk_status MCDK_CALL (*execute_python)(mcdk_handle self, mcdk_str code,
                                            mcdk_side side, uint32_t timeout_ms,
                                            mcdk_str* out_result_json);

    /* 截图：宿主持有句柄，插件拷走后必须 release */
    mcdk_status MCDK_CALL (*capture_window)(mcdk_handle self,
                                            const mcdk_capture_options* options,
                                            mcdk_handle* out_image);
    mcdk_status MCDK_CALL (*image_get_info)(mcdk_handle self, mcdk_handle image,
                                            mcdk_image_info* out_info);
    mcdk_status MCDK_CALL (*image_copy)(mcdk_handle self, mcdk_handle image,
                                        void* buffer, size_t capacity,
                                        size_t* out_written);
    void        MCDK_CALL (*image_release)(mcdk_handle self, mcdk_handle image);
} mcdk_iface_game;
```

### 6.1 `execute_python`

底层是现有的 `ipc_code_execution::requestCodeReturnValueJson`。`timeout_ms` 取值被钳制到 `[1, 120000]`，传 0 表示用宿主默认（10000）。

游戏未进入世界或调试 IPC 无客户端时返回 `MCDK_ERR_GAME_NOT_READY`；超时返回 `MCDK_ERR_TIMEOUT`。

**规范：禁止在 `MCDK_DISPATCH_SYNC` 派发的事件处理器中调用 `execute_python`。**

尤其是 `mcdk.log.line`：该回调运行在日志读取线程上，阻塞它会卡住整条游戏日志管道；而 Python 执行本身又会产生日志，构成自锁。需要在收到日志后执行代码，必须改用 `MCDK_DISPATCH_QUEUED` 或经由 `post_main` 转手。

`execute_python` 可安全地在 MCP 工具 handler 中调用——那是它最主要的用法。

### 6.2 截图

`capture_window` 底层是现有的 `window_capture`，输出 JPEG。流程固定为四步：

```c
mcdk_handle img = 0;
mcdk_capture_options opts = {0};
opts.struct_size = sizeof(opts);
opts.max_height  = 720;
if (game->capture_window(self, &opts, &img) == MCDK_OK) {
    mcdk_image_info info = {0};
    info.struct_size = sizeof(info);
    game->image_get_info(self, img, &info);
    /* 插件自己分配 info.byte_size 字节 */
    size_t written = 0;
    game->image_copy(self, img, buffer, capacity, &written);
    game->image_release(self, img);
}
```

采用宿主持有句柄而非共享分配器，是为了让边界上不出现任何分配器穿越（见 [02-abi-contract.md](02-abi-contract.md) §6）。`capacity` 小于 `byte_size` 时 `image_copy` 不写入任何数据，回填所需长度并返回 `MCDK_ERR_BUFFER_TOO_SMALL`。

SDK 把这四步收成了一个 `ctx.game().capture()`，直接返回 `CapturedImage`（含 `std::vector<std::uint8_t>`），句柄在函数内部就释放干净了——用户没有机会忘掉 `image_release`。

#### 实现时对本节的两处修正

- **截取范围改成归一化 `double`。** 原本写的是像素的 `region_x/y/w/h`，但插件拿不到客户区尺寸（它既不持有窗口句柄，也不该持有），那组参数根本无法被正确使用。底层 `MCDevTool::Style::CaptureRegion` 本来就是归一化的，与 `mc_input` 同构。
- **去掉了 `jpeg_quality`。** 底层 `CaptureOptions` 没有这个旋钮，留一个恒不生效的字段是陷阱。结构体带 `struct_size`，将来真支持了再追加即可。

**规范：`image_release` 必须被调用。** 宿主**应该**在 `MCDK_STAGE_SHUTDOWN` 时清理该插件遗留的全部图像句柄并就每个泄漏项打印警告——不能因为插件忘记 release 就让内存留到进程结束。

非 Windows 平台上 `capture_window` 返回 `MCDK_ERR_NOT_SUPPORTED`。

## 7. `mcdk.log/1`

```c
typedef uint32_t mcdk_log_channel;
enum { MCDK_LOG_CHANNEL_STDOUT = 0, MCDK_LOG_CHANNEL_STDERR = 1 };

typedef uint32_t mcdk_log_order;
enum { MCDK_LOG_ORDER_DESC = 0,   /* 由新到旧 */
       MCDK_LOG_ORDER_ASC  = 1 }; /* 由旧到新 */

typedef struct mcdk_log_query {
    uint32_t         struct_size;
    mcdk_log_channel channel;
    uint32_t         start_index;  /* 相对最新条目的索引，0 = 最新一条，含 */
    uint32_t         end_index;    /* 不含；等于 start_index 时改用 max_count 截断 */
    uint32_t         max_count;    /* 0 = 不限，仍受缓冲区容量约束 */
    mcdk_log_order   order;
} mcdk_log_query;

typedef struct mcdk_log_entry {
    uint32_t struct_size;
    uint32_t index;                /* 相对最新条目的索引 */
    int64_t  timestamp_ms;
    mcdk_str text;                 /* 借用，sink 返回即失效 */
} mcdk_log_entry;

typedef void (MCDK_CALL *mcdk_log_sink)(void* user, const mcdk_log_entry* entry);

typedef struct mcdk_iface_log {
    uint32_t struct_size;
    mcdk_status MCDK_CALL (*query)(mcdk_handle self, const mcdk_log_query* query,
                                   mcdk_log_sink sink, void* user);
    uint32_t    MCDK_CALL (*count)(mcdk_handle self, mcdk_log_channel channel);
} mcdk_iface_log;
```

索引语义与现有 MCP 工具 `get_latest_logs` / `get_log_range` 完全一致：**索引 0 是最新一条**，1 是次新，以此类推。底层是现有的 `LogBuffer`（stdout 与 stderr 各一份环形缓冲）。

**`timestamp_ms` 在 v1 恒为 0。** `LogBuffer` 只存文本，不存逐条时间戳，而为了这个字段去改现有缓冲区的存储形态不值得。字段保留是因为它将来会有值，而结构体布局一旦发布就只能追加。**需要时间戳的插件应订阅 `mcdk.log.line`**——那条路径上的 payload 带真实时间戳。日志行文本本身也带着游戏输出的时间。

### 7.1 sink 回调约束（规范）

采用回调式枚举而非返回数组，是为了让日志文本保持借用、不产生任何跨界分配。代价是对 sink 有严格要求：

**宿主在持有 `LogBuffer` 锁的状态下逐条调用 sink。因此：**

- sink **必须**极短，只做拷贝或匹配；
- sink 内**禁止**调用任何其他 mcdk 接口——包括 `mcdk.console`，会死锁；
- sink 内**禁止**阻塞、等待其他线程、或抛出异常（SDK 屏障会兜住异常，但该条目会被跳过）。

需要复杂处理时，先在 sink 里把文本拷进插件自己的容器，`query` 返回后再处理。SDK 的 `ctx.log().query(...)` 默认就是这么做的，直接返回 `std::vector<LogEntry>`，用户拿不到裸 sink。

## 8. `mcdk.mcp/1`

MCP 工具的描述是这套 ABI 上最复杂的一个数据结构——`mcp::tool` 含两棵 JSON 树和五个 `std::optional`，它是**复杂类型降级规则的样板**，见 §8.1。

```c
/* 注解位掩码。禁止用位域（见 02 §5.4），用显式掩码常量 */
typedef uint32_t mcdk_mcp_annotation;
enum {
    MCDK_MCP_ANNOTATION_READ_ONLY   = 1u << 0,
    MCDK_MCP_ANNOTATION_DESTRUCTIVE = 1u << 1,
    MCDK_MCP_ANNOTATION_IDEMPOTENT  = 1u << 2,
    MCDK_MCP_ANNOTATION_OPEN_WORLD  = 1u << 3,
};

typedef struct mcdk_mcp_tool_desc {
    uint32_t struct_size;
    /* 哪些注解被显式设置（对应 C++ 侧 std::optional 的 has_value） */
    uint32_t annotation_present;
    /* 被设置的那些注解各自的值；未在 present 中置位的位无意义 */
    uint32_t annotation_value;
    uint32_t _reserved;
    mcdk_str name;
    mcdk_str description;
    mcdk_str title;               /* annotations.title；len == 0 表示未设置 */
    mcdk_str input_schema_json;   /* JSON 文本，必填 */
    mcdk_str output_schema_json;  /* JSON 文本；len == 0 表示未设置 */
} mcdk_mcp_tool_desc;

/* 返回非 MCDK_OK 时，宿主从插件错误槽取消息转成 MCP 错误响应 */
typedef mcdk_status (MCDK_CALL *mcdk_mcp_tool_handler)(
    void*    user,
    mcdk_str arguments_json,   /* 借用 */
    mcdk_str session_id,       /* 借用，MCP 会话标识 */
    mcdk_str* out_result_json  /* 指向插件侧 TLS，宿主立即拷贝，见 §8.4 */
);

typedef struct mcdk_iface_mcp {
    uint32_t struct_size;
    mcdk_status MCDK_CALL (*add_tool)(mcdk_handle                 self,
                                      const mcdk_mcp_tool_desc*   desc,
                                      mcdk_mcp_tool_handler       handler,
                                      void*                       user);
    /* 当前已注册的全部工具，JSON 数组文本。借用 */
    mcdk_status MCDK_CALL (*list_tools)(mcdk_handle self, mcdk_str* out_json);
} mcdk_iface_mcp;
```

### 8.1 复杂类型的降级规则（规范）

`mcp::tool` 里的每一类 C++ 构造在边界上都有固定的降级形式。**这张表适用于所有接口，不止 MCP**：

| C++ 侧 | 边界形态 | 本例 |
| --- | --- | --- |
| `nlohmann::json`（任意嵌套） | `mcdk_str`，UTF-8 JSON 文本 | `input_schema_json`、`output_schema_json`、`arguments_json` |
| `std::string` | `mcdk_str` | `name`、`description` |
| `std::optional<std::string>` | `mcdk_str`，`len == 0` 即未设置 | `title` |
| `std::optional<bool>` × N | 两个 `uint32_t` 位掩码：present + value | 四个 hint |
| `std::function` | 函数指针 + `void* user` | `handler` |
| 可增长的参数组 | 带 `struct_size` 的描述结构体，**不用位置参数** | `mcdk_mcp_tool_desc` |

最后一行是这次修正的由来：`add_tool` 原本写成位置参数 `(name, description, schema_json, ...)`，无法表达 `output_schema` 与 `annotations`，将来补就只能新开 `add_tool2`。**凡是参数会随上游结构增长的接口，一律用带 `struct_size` 的描述结构体**，这样加字段是追加而非换函数。

**禁止**把 JSON 以任何二进制/句柄形式过界。文本形态虽有序列化开销，但工具注册是一次性的、参数调用是低频的（相对日志而言），换来的是零 ABI 耦合——插件用什么 JSON 库、什么版本，宿主完全不需要知道。

### 8.2 注册窗口

`add_tool` 只能在 `MCDK_STAGE_REGISTER` 阶段调用，通常写在 `mcdk.mcp.register.before` 事件处理器里。注册表封存后调用返回 `MCDK_ERR_WRONG_STAGE`。

**工具名冲突返回 `MCDK_ERR_DUPLICATE`，禁止后注册者覆盖先注册者。** 否则插件的加载顺序会悄悄改变 AI 看到的工具语义，这类问题在排查时几乎无迹可循。插件**应该**给自己的工具名加可辨识的前缀。

`input_schema_json` 必须是合法的 JSON Schema 对象文本；宿主在注册时校验，非法则返回 `MCDK_ERR_INVALID_ARGUMENT` 并在错误槽给出解析位置。`output_schema_json` 非空时同样校验。

### 8.3 handler 的线程与并发（规范）

- handler 运行在 **MCP 工作线程**，不是主线程；
- **可能被并发调用**——现有 `RpcMethodOptions` 的默认 `maxConcurrency` 是 8，handler **必须**自行保证线程安全；
- **允许阻塞**，这正是 `execute_python` 的主要调用场景（见 §6.1，该处禁止的是 SYNC 事件处理器，不是这里）；
- v1 **不提供**取消令牌，handler 需自行限制耗时。超时由 MCP 层判定，但超时后 handler 仍会跑完。

### 8.4 结果的所有权（规范）

`out_result_json` 指向**插件侧**线程局部缓冲，宿主**必须**在 handler 返回后立即拷贝。这与错误槽（§3）是同一套机制，只是方向相反：谁产生数据谁用自己的 TLS 暂存，对方立即拷走，两边都不分配跨界内存。

SDK 自动处理这一层：用户的 handler 直接 `return nlohmann::json{...}`，SDK 序列化后存进 TLS 再回填指针。

## 9. 阶段与线程矩阵

可调用阶段（阶段定义见 [03-abi-reference.md](03-abi-reference.md) §4），在错误阶段调用**必须**返回 `MCDK_ERR_WRONG_STAGE`：

| 接口 | REGISTER | CONFIG | WORLD | RUNTIME | SHUTDOWN |
| --- | :-: | :-: | :-: | :-: | :-: |
| `mcdk.core` | ✓ | ✓ | ✓ | ✓ | ✓ |
| `mcdk.console` | ✓ | ✓ | ✓ | ✓ | ✓ |
| `mcdk.info` | ✓ | ✓ | ✓ | ✓ | ✓ |
| `mcdk.events` 订阅 | ✓ | ✓ | ✓ | ✓ | — |
| `mcdk.mcp` 注册 | ✓ | — | — | — | — |
| `mcdk.log` | — | — | — | ✓ | ✓ |
| `mcdk.game` | — | — | — | ✓ | — |

线程约束汇总：

| 接口 | 可调用线程 | 阻塞 |
| --- | --- | --- |
| `mcdk.core` | 任意 | 否 |
| `mcdk.console` | 任意 | 否（内部加锁，极短） |
| `mcdk.info` | 任意 | 否 |
| `mcdk.mcp` 注册 | 仅 REGISTER 阶段所在的主线程 | 否 |
| `mcdk.log.query` | 任意，但不得在 sink 或另一个 `query` 内重入 | 是（持锁） |
| `mcdk.game.execute_python` | 任意，**除 SYNC 事件处理器外**（见 §6.1） | 是（至多 `timeout_ms`） |
| `mcdk.game.capture_window` | 任意 | 是（数十毫秒量级） |

## 10. 明确不在 v1 范围

以下能力在前几版设计稿中出现过，现确认**推迟**，待 v1 落地并有真实插件需求后再评估：

| 能力 | 推迟原因 |
| --- | --- |
| `mcdk.config` 读取插件私有配置 / 修改 `UserConfig` | 白名单范围未定，见 [10-roadmap.md](10-roadmap.md) §4 待定问题 3 |
| `mcdk.paths` | 所需路径已并入 `mcdk.info` 的 `mcdk_session_info` |
| `mcdk.rpc` 注册 Host Bridge 方法 | 面向 IDE，与 MCP 能力重叠，等 MCP 路径验证后再开 |
| `mcdk.hotreload` 注册自定义 watcher | 需要先确定通配语法与 `IncrementalReloadWatcherTask` 的复用边界 |
| `mcdk.pack` 干预 Pack 清单 | 涉及 `WORLD` 阶段的写盘时序，风险高于收益 |
| `mcdk.task` 定时器与后台 job | 插件可自建线程，`post_main` 已由 `mcdk.events` 提供 |
| `mcdk.store` 私有 KV | 插件可自行读写 `mcdk_session_info::project_root` 下的文件 |
| `mcdk.mem` 共享分配器 | v1 靠句柄 + 调用方缓冲解决，不引入分配器（见 [02](02-abi-contract.md) §6） |
| `mcdk.core::register_interface` 插件间接口 | 依赖拓扑排序已在 [06-loading.md](06-loading.md) 设计，但 v1 无消费方 |
| 窗口输入注入 | 与截图同属窗口操作，但缺少明确的插件侧用例 |

推迟不等于放弃：这些接口的形状已在前述文档中留档，按 [02-abi-contract.md](02-abi-contract.md) §8 的追加规则，将来新增接口表不会影响已冻结的部分。

## 11. SDK 侧映射

用户看到的 C++ 形态，与上述 C 定义一一对应：

```cpp
void onRegister(mcdk::Context& ctx) override {
    ctx.console().info("上线");
    ctx.console().print(mcdk::Color::Cyan, "带颜色的一行");

    ctx.events().on<mcdk::ev::McpRegisterBefore>([&](const auto&) {
        ctx.mcp().addTool("my_tool", "说明", schema,
            [&ctx](const nlohmann::json& args) -> nlohmann::json {
                // MCP 工作线程上，允许阻塞
                return ctx.game().executePython(args["code"], mcdk::Side::Server);
            });
    });
}

void onRuntime(mcdk::Context& ctx) override {
    const auto s = ctx.info().session();          // 内含 std::string，可长期持有
    ctx.console().info("MCP 端口 " + std::to_string(s.mcpPort)
                     + "，游戏 IPC 端口 " + std::to_string(s.gameIpcPort));

    auto logs = ctx.log().latest(mcdk::LogChannel::Stdout, 50);  // std::vector<LogEntry>
    auto shot = ctx.game().captureWindow({.maxHeight = 720});    // std::vector<std::byte>，RAII 释放句柄
}
```

`captureWindow` 返回的对象在析构时自动调用 `image_release`，用户不会忘。
