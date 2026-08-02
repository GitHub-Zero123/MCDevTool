# MCDK Host Bridge Protocol v1

状态：Draft v1

本文定义 VSCode 扩展或其他桌面客户端与 MCDK 之间的本地双向通信协议，以及 MCDK 内部用于注册远程可调用函数的 C++ 接口。客户端负责监听，MCDK 负责主动连接。

文中的“必须”“禁止”“应该”“可以”分别对应 MUST、MUST NOT、SHOULD、MAY。除明确标记为可选的字段外，双方不得依赖本文未定义的行为。

## 1. 背景与边界

当前项目已有三层通信能力：

1. `DebugIPCServer` 在 MCDK 内监听随机 loopback TCP 端口，游戏内调试 MOD 主动连接该端口。
2. `DebugIPCServer::requestJson()` 已支持通过请求 ID 等待游戏内 Python 返回 JSON 结果；旧的数字消息类型用于不等待结果的热更新和代码执行。
3. MCP Server 和 `mcdk_stdio_bridge` 面向 AI/MCP 客户端，MCP 调用最终仍会进入日志、窗口操作或游戏 IPC。

Host Bridge 是独立的 IDE 控制面，不替代游戏 IPC，也不替代 MCP：

```text
VSCode / 其他 Host
  TCP listener on 127.0.0.1:<MCDEV_HOST_PORT>
              ^
              | MCDK 主动建立的持久全双工连接
              v
            MCDK
              ^
              | 现有 DebugIPCServer 私有连接
              v
       Minecraft 调试 MOD
```

Host 不得直接连接 `DebugIPCServer`。初始化信息中的游戏 IPC 端口只用于状态展示和诊断，业务请求必须发给 MCDK。这样可以避免外部连接被误认为游戏调试 MOD，也避免破坏当前“JSON 请求发送给第一个 IPC 客户端”的语义。

## 2. 设计目标

v1 必须满足：

- 客户端先监听，再通过环境变量把端口和认证令牌传给它启动的 MCDK。
- MCDK 主动连接客户端，因此不需要为每个 MCDK 实例配置固定监听端口。
- 同一条连接同时支持通知、请求和响应。
- 请求和响应可并发，响应可乱序，通过 ID 关联。
- 明确区分“Minecraft 进程已启动”和“游戏内 IPC 已就绪”。
- 客户端重启或 Extension Host reload 后，MCDK 可以在 Minecraft 仍运行时重连。
- 一个 Host listener 可以同时接受多个 MCDK 实例。
- C++ 业务函数和网络收发解耦，注册后可被 Host 调用，也可由其他适配层复用。

v1 不处理：

- 跨机器连接。
- 高带宽视频、截图流或逐帧遥测。
- Host 直连游戏内 Python IPC。
- 请求在断线后的自动重放。
- JSON-RPC batch。

## 3. 角色与术语

- **Host**：VSCode 扩展或其他客户端进程。它创建 TCP listener，并启动 MCDK。
- **MCDK**：TCP 主动连接方，同时也是业务 RPC 的主要服务方。
- **Game IPC**：MCDK 与 Minecraft 调试 MOD 之间已有的私有协议。
- **会话**：一次 MCDK 进程及其启动的 Minecraft 进程的完整生命周期。
- **连接代次**：同一会话每次重新连接 Host 时递增的整数。
- **通知**：没有 `id` 的 JSON-RPC 消息。接收方禁止返回响应。
- **请求**：带非空 `id` 的 JSON-RPC 消息。接收方必须返回结果或错误。

## 4. 启用与发现

### 4.1 环境变量

Host 启动 MCDK 前必须设置：

| 变量 | 格式 | 含义 |
| --- | --- | --- |
| `MCDEV_HOST_PORT` | 十进制整数，`1..65535` | Host 在 `127.0.0.1` 上监听的 TCP 端口 |
| `MCDEV_HOST_TOKEN` | 64 个十六进制字符 | Host 使用 CSPRNG 为本次 MCDK 会话生成的 32 字节认证密钥 |

未设置 `MCDEV_HOST_PORT` 时，Host Bridge 必须完全禁用，不得改变普通命令行启动行为。

设置了端口但端口或令牌格式无效时，MCDK 必须输出明确错误并禁用 Host Bridge。MCDK 不得因为可选的 Host Bridge 配置错误而阻止 Minecraft 启动。

Host 应为每个启动的 MCDK 生成不同密钥，并在该 MCDK 的整个生命周期内保留密钥，以支持断线重连。Extension Host reload 后仍要管理已有 MCDK 时，端口和密钥必须保存在可恢复的安全状态中，不能只保存在进程内存里。

v1 的地址固定为 IPv4 `127.0.0.1`，禁止从环境变量接受非 loopback 地址。未来如需 named pipe 或其他传输，应新增 endpoint 变量并提升应用协议版本，不改变 v1 语义。

### 4.2 环境变量隔离

MCDK 当前构造 Minecraft 子进程环境块时会复制自己的完整环境。实现 Host Bridge 时，子进程环境构造器必须移除 `MCDEV_HOST_PORT` 和 `MCDEV_HOST_TOKEN`，再加入游戏需要的 `MCDEV_DEBUG_IPC_PORT`。

认证令牌只允许存在于 Host 和 MCDK 中，不应传递给 Minecraft 或玩法脚本。

## 5. 传输与分帧

### 5.1 TCP 连接

- Host 必须监听 `127.0.0.1:MCDEV_HOST_PORT`。
- MCDK 必须主动建立 TCP 连接。
- 连接建立后是持久、全双工字节流。
- v1 不使用 TLS。安全边界由 loopback 限制和握手令牌共同提供。
- 双方应该启用 `TCP_NODELAY`。
- Windows Host 应使用 `SO_EXCLUSIVEADDRUSE`，防止其他本地进程同时占用或劫持 listener 端口。

### 5.2 帧格式

TCP 不保留消息边界。每个消息使用以下格式：

```text
+----------------------+-------------------------------+
| payload_length       | payload                       |
| uint32, big-endian   | payload_length bytes          |
+----------------------+-------------------------------+
```

- `payload_length` 是 UTF-8 JSON 的字节数，不是字符数。
- 长度必须在 `1..16777216` 之间，即最大 16 MiB。
- payload 必须是一个 UTF-8 编码的 JSON object。
- v1 禁止压缩、JSON array/batch 和尾随字节。
- 接收器必须正确处理半包、粘包，以及 header 和 payload 被任意拆分的情况。
- 长度为 0、超过上限、非法 UTF-8 或非法 JSON 时，接收方必须关闭当前连接。

规范测试向量：

```text
JSON:   {"jsonrpc":"2.0","id":1,"method":"mcdk/ping","params":{}}
Bytes:  57
Header: 00 00 00 39
Frame:  [00 00 00 39] + 上述 57 个 ASCII/UTF-8 字节
```

## 6. JSON-RPC 消息模型

v1 使用 JSON-RPC 2.0 envelope，并增加本文定义的握手、生命周期和限流规则。

### 6.1 请求

```json
{
  "jsonrpc": "2.0",
  "id": "host:42",
  "method": "game/player/get",
  "params": {
    "playerId": "123"
  }
}
```

- `id` 必须是非空字符串或不含小数的 JSON number，禁止为 `null`。
- number ID 必须位于有符号 64 位整数范围内。超出范围或使用浮点数的 ID 属于 `INVALID_REQUEST`。
- 发送方在同一连接中存在未完成请求时，不得复用该 `id`。
- 推荐 Host 使用 `host:<counter>`，MCDK 使用 `mcdk:<counter>`，便于日志定位。
- `params` 缺省时按空 object 处理。具体方法可以声明其他 JSON 类型。

### 6.2 成功响应

```json
{
  "jsonrpc": "2.0",
  "id": "host:42",
  "result": {
    "name": "developer"
  }
}
```

### 6.3 错误响应

```json
{
  "jsonrpc": "2.0",
  "id": "host:42",
  "error": {
    "code": -32011,
    "message": "Minecraft has not entered a world",
    "data": {
      "code": "GAME_WORLD_NOT_READY",
      "retryable": true,
      "sessionId": "550e8400-e29b-41d4-a716-446655440000",
      "state": "process_started",
      "minecraftPid": 12040,
      "debugCapabilityEnabled": true,
      "inWorld": false,
      "gameIpcClientCount": 0
    }
  }
}
```

响应必须且只能包含 `result` 或 `error` 之一。`error.data.code` 是供客户端稳定判断的符号错误码，客户端不应解析英文 `message`。

### 6.4 通知

```json
{
  "jsonrpc": "2.0",
  "method": "game/reload",
  "params": {
    "addons": false
  }
}
```

通知没有 `id`，接收方禁止响应。通知是 best-effort：发送方只能确认数据写入了本地 socket，不能确认业务执行成功。需要确认成功、错误或返回值时，必须发送带 `id` 的请求。

方法注册信息会声明该方法允许 `request`、`notification` 或两者。使用不允许的模式调用时，请求返回 `MODE_NOT_ALLOWED`；通知只记录诊断日志。

### 6.5 响应乱序

业务 handler 不保证按请求到达顺序完成。例如 `host:2` 的响应可以早于 `host:1`。双方必须只通过 `id` 关联响应，禁止依赖顺序。

## 7. 初始化握手

### 7.1 启动时机

MCDK 可以提前创建连接管理线程，但必须在以下信息都确定后才发送 `mcdk/initialize`：

- MCDK PID；
- Minecraft PID；
- Game IPC 监听端口；
- 本次启动的项目与世界信息。

因此 v1 的初始化请求应在 `CreateProcessW` 成功后发送。此时 Minecraft 内的调试 MOD 通常尚未连接，初始化状态一般是 `process_started`，不能错误报告为 `game_ready`。

### 7.2 MCDK 初始化请求

新 TCP 连接上的第一个 frame 必须是 MCDK 发出的 `mcdk/initialize` 请求：

```json
{
  "jsonrpc": "2.0",
  "id": "mcdk:1",
  "method": "mcdk/initialize",
  "params": {
    "protocol": {
      "minVersion": 1,
      "maxVersion": 1
    },
    "authToken": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
    "session": {
      "id": "550e8400-e29b-41d4-a716-446655440000",
      "connectionGeneration": 1,
      "startedAt": "2026-08-03T12:34:56.789Z",
      "state": "process_started",
      "stateSequence": 1
    },
    "mcdk": {
      "pid": 12000,
      "version": "0.1.0"
    },
    "minecraft": {
      "pid": 12040
    },
    "gameIpc": {
      "host": "127.0.0.1",
      "port": 49152,
      "connected": false
    },
    "project": {
      "root": "D:/workspace/example"
    },
    "world": {
      "name": "MC_DEV_WORLD",
      "folderName": "MC_DEV_WORLD",
      "runtimePath": "C:/Users/user/AppData/Local/.../minecraftWorlds/MC_DEV_WORLD",
      "sourcePath": null
    },
    "capabilities": {
      "methodDiscovery": true,
      "notifications": true,
      "cancellation": true,
      "debugCapabilityEnabled": true
    },
    "limits": {
      "maxFrameBytes": 16777216,
      "maxInFlightRequests": 64
    }
  }
}
```

字段规则：

- `session.id` 是 UUID。它在 MCDK 进程生命周期内保持不变，重连时仍使用原值。
- `connectionGeneration` 从 1 开始，每次建立新的 TCP 连接并尝试初始化时递增。
- `stateSequence` 在整个会话内单调递增，不因重连清零。
- PID 使用 JSON 非负整数。MCDK 和 Minecraft PID 必须大于 0。
- 时间使用 UTC RFC 3339，精确到毫秒。
- 路径必须是绝对路径、UTF-8 字符串，推荐统一输出 `/` 分隔符。
- `world.name` 是 MCDK 配置中的世界名。玩法地图直接部署已有 `level.dat` 时，它可能和文件内实际 `LevelName` 不同。
- `world.folderName` 是运行时世界目录名，也是自动进入游戏时使用的 `level_id`。
- `world.sourcePath` 在没有玩法地图源目录时为 JSON `null`。
- `gameIpc.port` 只用于诊断，Host 禁止连接该端口。
- `capabilities.debugCapabilityEnabled` 仅在本次启动包含调试 MOD 且内部 IPC Server 已创建时为 `true`。
- `limits` 描述 MCDK 作为接收方允许 Host 使用的上限。Host 响应中的 `limits` 则描述 Host 自己的接收上限。

### 7.3 Host 初始化响应

Host 必须先验证 TCP 对端来自 loopback，再使用常量时间比较验证 `authToken`。验证成功后，Host 选择双方都支持的最高协议版本并响应：

```json
{
  "jsonrpc": "2.0",
  "id": "mcdk:1",
  "result": {
    "protocolVersion": 1,
    "connectionId": "7e78b3e4-4978-4f26-bf01-e320fe85673a",
    "host": {
      "name": "example-vscode-extension",
      "version": "1.2.0",
      "instanceId": "vscode-window-3"
    },
    "heartbeatIntervalMs": 10000,
    "limits": {
      "maxFrameBytes": 16777216,
      "maxInFlightRequests": 64
    }
  }
}
```

- Host 必须在收到完整初始化请求后的 3 秒内响应。
- Host 在成功响应前不得发送任何其他请求或通知。
- `heartbeatIntervalMs` 必须为 `5000..60000`。MCDK 是默认心跳发起方：连接空闲一个周期后发送 `mcdk/ping`，连续 3 个心跳请求未收到响应时关闭并重连。Host 也可以主动调用 `mcdk/ping`。
- `maxFrameBytes` 不得高于 v1 固定上限。
- Host 应以 `(session.id, connectionGeneration)` 标识连接。更高代次初始化成功后，应关闭该会话的旧连接。
- 双方不得在响应、普通日志或 trace dump 中回显认证令牌；诊断输出必须统一替换为 `<redacted>`。

认证失败返回 `AUTH_FAILED`，版本无交集返回 `PROTOCOL_VERSION_UNSUPPORTED`，随后 Host 必须关闭连接。认证或版本错误属于永久错误，MCDK 不应在同一配置下循环重试；连接拒绝、连接重置和 Host 超时属于临时错误，可以重试。

## 8. 会话状态与生命周期通知

### 8.1 状态

| 状态 | 含义 |
| --- | --- |
| `process_started` | Minecraft 进程已创建，游戏内 IPC 尚未确认可用 |
| `game_ready` | 调试 MOD 已连接，即 `DebugIPCServer::getClientCount() > 0` |
| `game_unavailable` | Minecraft 仍运行，但调试 MOD 未包含、断开或不可用 |
| `exiting` | 已检测到 Minecraft 退出，MCDK 正在清理资源 |
| `exited` | 已取得 Minecraft 退出码，即将关闭 Host Bridge |

### 8.2 状态通知

初始化成功后，每次状态变化，MCDK 必须发送：

```json
{
  "jsonrpc": "2.0",
  "method": "mcdk/session/stateChanged",
  "params": {
    "sessionId": "550e8400-e29b-41d4-a716-446655440000",
    "sequence": 2,
    "state": "game_ready",
    "timestamp": "2026-08-03T12:35:01.120Z",
    "gameIpcConnected": true,
    "reason": null,
    "minecraftExitCode": null
  }
}
```

- `sequence` 必须严格递增。
- Host 应忽略 `sequence` 小于或等于已处理值的状态通知。
- 断线期间的通知不重放。重连后的 `mcdk/initialize` 必须携带当前完整状态和最新 `stateSequence`。
- `exited` 通知中的 `minecraftExitCode` 必须是从进程句柄取得的实际退出码。
- 发送 `exited` 后，MCDK 应尽力等待最多 500 ms 让发送队列清空，然后关闭连接；不得因此长时间阻塞进程退出。

## 9. 核心方法

以下方法属于协议核心，MCDK 必须注册。除 `mcdk/initialize` 外，它们只能在初始化成功后调用。

### 9.1 `mcdk/ping`

请求参数可以为空 object，成功结果至少包含接收时间：

```json
{
  "receivedAt": "2026-08-03T12:35:10.000Z"
}
```

Host 和 MCDK 都必须处理该请求。它用于心跳和连接活性检测，不代表 Game IPC 已就绪。

### 9.2 `mcdk/session/get`

方向：Host 到 MCDK，请求模式。

返回与初始化请求相同的会话、进程、Game IPC、项目和世界快照，但不包含 `authToken`。

### 9.3 `mcdk/methods/list`

方向：Host 到 MCDK，请求模式。

结果：

```json
{
  "methods": [
    {
      "name": "game/player/get",
      "modes": ["request"],
      "gameAvailability": "in_world",
      "paramsSchema": {
        "type": "object",
        "required": ["playerId"]
      },
      "resultSchema": {
        "type": "object"
      }
    }
  ]
}
```

- 结果必须按方法名升序排列，便于快照测试。
- schema 使用 JSON Schema 2020-12 的子集，可以为 `null`，表示没有公开 schema。
- 方法发现只描述能力，服务端仍必须在调用时验证参数。

### 9.4 `mcdk/cancel`

方向：任一端到另一端，通知模式。

```json
{
  "jsonrpc": "2.0",
  "method": "mcdk/cancel",
  "params": {
    "id": "host:42"
  }
}
```

接收方应请求停止对应 handler，并禁止在取消成功后再发送正常结果。底层 Minecraft API 不一定可中断，因此取消是协作式的；无法中断的工作可以继续清理，但其结果必须丢弃。请求 ID 不存在或已经完成时，接收方静默忽略该通知。

### 9.5 建议的首批业务绑定

以下名称不是传输层保留方法，但建议首个实现直接映射现有能力：

| 方法 | 模式 | 执行策略 | 现有能力来源 |
| --- | --- | --- | --- |
| `game/ping` | request | `game_serial` | Game IPC `ping` |
| `game/code/execute` | request、notification | `game_serial` | Game IPC `execute_code` 或旧消息 3/4 |
| `game/reload` | request、notification | `game_serial` | 旧消息 5/8 |
| `game/ui/reload` | request、notification | `worker` | Minecraft 窗口 UI reload shortcut |
| `logs/latest` | request | `worker` | `LogBuffer` |
| `logs/errors/latest` | request | `worker` | error `LogBuffer` |

### 9.6 游戏转发方法的存档就绪判定

注册方法只要会把调用转发给游戏内 IPC，就必须声明 `gameAvailability`。允许值如下：

| 值 | 调用前置条件 |
| --- | --- |
| `none` | 不依赖 Minecraft，可直接调用 |
| `debug_enabled` | 本次启动已包含调试 MOD，并创建了内部 IPC Server |
| `in_world` | 调试能力已开启，且 `DebugIPCServer::getClientCount() > 0` |

现有代码已经使用 `ipcServer->getClientCount() == 0` 判断“玩家可能尚未进入游戏或目标不可用”。游戏调试 MOD 的 `_IPCSYSTEM.start()` 在 `ON_CLIENT_INIT()` 中执行，因此 v1 直接使用 client count 作为存档状态判据：大于 0 表示已进入存档，等于 0 表示当前不在存档内。

dispatcher 必须在调用 handler 前按以下顺序检查，并快速返回，不得把一个已知无效调用送入 Game IPC 后再等待超时：

1. 本次启动没有包含调试 MOD 或没有创建内部 IPC Server：返回 `DEBUG_CAPABILITY_DISABLED`。
2. 调试能力已开启，但 `getClientCount() == 0`：返回 `GAME_WORLD_NOT_READY`。
3. 前置条件通过后才进入 handler。检查后发生竞争性断线时同样返回 `GAME_WORLD_NOT_READY`。

所有转发给游戏内 IPC 的代码执行、数据查询和游戏命令都注册为 `in_world`。`isClient` 只决定转发目标，不再产生额外的客户端或服务端就绪错误码。

游戏可用性错误的 `error.data` 必须包含：

- `code`：稳定的符号错误码；
- `retryable`：当前会话内稍后重试是否可能成功；
- `sessionId`、`state` 和 `minecraftPid`；
- `debugCapabilityEnabled`、`inWorld` 和 `gameIpcClientCount`。

游戏可用性只允许以下两个 `data.code`：

| JSON-RPC code | `data.code` | `retryable` | 含义 |
| ---: | --- | --- | --- |
| `-32010` | `DEBUG_CAPABILITY_DISABLED` | `false` | 本次启动未开启调试能力，当前会话内不会自行恢复 |
| `-32011` | `GAME_WORLD_NOT_READY` | `true` | 调试能力已开启，但当前不在游戏存档内 |

Minecraft 已退出但 Host Bridge 尚未关闭的极短窗口也统一视为 `GAME_WORLD_NOT_READY`。内部 IPC 发送失败、连接刚刚断开等竞争状态同样折叠为该错误。调用已经进入 handler 后发生的真正超时使用通用 `HANDLER_TIMEOUT`，不增加第三种游戏可用性错误。

请求模式按上面规则返回错误响应。通知模式没有 `id`，根据 JSON-RPC 规则不能返回错误；MCDK 必须拒绝执行并记录包含方法名和相同状态字段的结构化诊断。前端需要展示失败原因或决定是否重试时，必须使用请求模式。

## 10. 错误码

| JSON-RPC code | `data.code` | 含义 |
| ---: | --- | --- |
| `-32700` | `PARSE_ERROR` | JSON 解析失败；通常直接断开连接 |
| `-32600` | `INVALID_REQUEST` | envelope 不合法 |
| `-32601` | `METHOD_NOT_FOUND` | 方法未注册 |
| `-32602` | `INVALID_PARAMS` | 参数类型或字段不合法 |
| `-32603` | `INTERNAL_ERROR` | 未分类的服务端异常 |
| `-32001` | `AUTH_FAILED` | 初始化令牌错误 |
| `-32002` | `PROTOCOL_VERSION_UNSUPPORTED` | 协议版本无交集 |
| `-32003` | `NOT_INITIALIZED` | 握手前发送了其他消息 |
| `-32004` | `SERVER_BUSY` | 并发数或队列达到上限 |
| `-32005` | `MODE_NOT_ALLOWED` | 方法不支持当前请求/通知模式 |
| `-32010` | `DEBUG_CAPABILITY_DISABLED` | 本次启动未开启调试能力 |
| `-32011` | `GAME_WORLD_NOT_READY` | 调试能力已开启，但当前不在游戏存档内 |
| `-32013` | `REQUEST_CANCELLED` | 请求已取消 |
| `-32014` | `HANDLER_TIMEOUT` | MCDK handler 超时 |

业务方法可以在 `-32100..-32199` 范围增加错误，必须同时提供稳定的 `data.code`。

通知发生错误时没有响应。实现应该写诊断日志，但禁止为了报告通知错误而构造一个伪响应。

## 11. 并发、限流与超时

- 每个方向默认最多允许 64 个未完成请求。
- 发送方不得超过对端在初始化时公布的 `maxInFlightRequests`。
- 超过连接级并发限制时，请求立即返回 `SERVER_BUSY`。
- 工作队列必须有界，禁止为每个请求无上限创建线程。
- 网络读取线程不得执行会阻塞的业务 handler。
- socket 写入必须串行化，禁止多个线程交错写 frame。
- handler 响应可以乱序。
- 每个方法必须配置默认超时。普通方法建议 10 秒，纯内存查询建议 2 秒。
- 超时后发送 `HANDLER_TIMEOUT`，迟到结果必须丢弃。
- 无法强制中断的 handler 在实际返回前继续占用方法并发配额，避免连续超时绕过限流。
- 响应发送队列达到上限时不得静默丢弃响应；实现必须关闭连接，使双方明确失败。可丢弃的遥测必须使用未来单独定义的流控通知。
- 进入 `exiting` 后，新的游戏业务请求返回 `GAME_WORLD_NOT_READY`。

现有游戏内 Python IPC 在读取线程中等待主线程 timer 完成，因此 v1 对所有进入 `DebugIPCServer::requestJson()` 的方法使用单独的 `game_serial` 执行队列，同一时刻最多一个游戏请求。待内部 IPC 明确支持并发后，才能提升该限制。

## 12. 断线与重连

MCDK 负责重连，Host 负责持续监听。

- 初次连接失败或活动连接意外断开时，MCDK 使用指数退避：`100 ms, 200 ms, 500 ms, 1 s, 2 s, 5 s`，之后保持 5 秒，可加入最多 20% jitter。
- Minecraft 退出或 MCDK 开始停止后，不再重连。
- 认证失败和协议版本不支持不重试。
- 重连时重新执行完整初始化，并递增 `connectionGeneration`。
- 断线时所有未完成请求立即在本地失败，不自动重放。
- Host 不得自动重放请求或通知，因为 `game/reload`、代码执行等操作不是幂等的。
- Host 在重连后调用 `mcdk/session/get` 或直接使用初始化快照恢复 UI 状态。

v1 的目标端口只在 MCDK 启动时通过环境变量传入，运行中不能切换。Host 服务重启时必须重新绑定原端口，并恢复该会话的认证密钥；如果客户端无法保证这一点，应让一个生命周期独立于 Extension Host 的本地 broker 持有 listener。动态迁移端口需要后续版本增加稳定发现机制。

## 13. C++ RPC 绑定注册接口

### 13.1 设计原则

注册接口应满足：

- 方法名到 handler 的平均 O(1) 查找。
- 支持 raw JSON handler 和强类型 handler。
- 重复方法名在启动阶段立即报错，不允许静默覆盖。
- 注册完成后 `seal()`，热路径无注册锁。
- handler 不持有 registry 锁执行。
- 参数转换、异常到 RPC error 的转换、超时和响应序列化由框架统一处理。
- transport 不包含 Minecraft 业务逻辑；MCP 和 Host Bridge 可以复用同一 service/handler 层。

### 13.2 建议头文件接口

以下是接口约定，不要求逐字采用成员命名，但语义必须保持一致：

```cpp
namespace mcdk::bridge {

using Json = nlohmann::json;

struct RpcError {
    int         code;
    std::string message;
    Json        data = Json::object();
};

using RpcResult = std::expected<Json, RpcError>;

enum class RpcMode : std::uint8_t {
    Request      = 1,
    Notification = 2,
};

enum class ExecutionPolicy {
    Inline,      // 仅允许无阻塞、无 I/O 的内部快照查询
    Worker,      // 有界通用工作池
    GameSerial,  // 单线程串行进入现有 Game IPC
};

enum class GameAvailability {
    None,
    DebugEnabled,
    InWorld,
};

struct GameStateSnapshot {
    std::string   sessionId;
    std::string   state;
    std::uint32_t minecraftPid = 0;
    bool          debugCapabilityEnabled = false;
    std::size_t   ipcClientCount = 0;

    [[nodiscard]] bool inWorld() const noexcept { return debugCapabilityEnabled && ipcClientCount > 0; }
};

class GameAvailabilityGuard {
public:
    [[nodiscard]] GameStateSnapshot snapshot() const;

    // 返回 nullopt 表示满足条件，否则返回已经包含标准 data 字段的 RPC error。
    [[nodiscard]] std::optional<RpcError> check(GameAvailability required) const;
};

struct MethodOptions {
    std::uint8_t             modes = static_cast<std::uint8_t>(RpcMode::Request);
    ExecutionPolicy          execution = ExecutionPolicy::Worker;
    GameAvailability         gameAvailability = GameAvailability::None;
    std::chrono::milliseconds timeout{10000};
    std::uint32_t            maxConcurrency = 8;
};

struct MethodDescriptor {
    std::string name;
    Json        paramsSchema = nullptr;
    Json        resultSchema = nullptr;
};

struct MethodEntry;

using RpcId = std::variant<std::int64_t, std::string>;

struct RpcContext {
    std::string_view                      method;
    std::optional<RpcId>                  id; // notification 时为 nullopt
    bool                                  notification;
    std::chrono::steady_clock::time_point deadline;
    std::stop_token                       stopToken;
    std::string_view                      sessionId;
};

using RawHandler = std::function<RpcResult(const RpcContext&, const Json&)>;

enum class BindError {
    InvalidName,
    DuplicateName,
    InvalidOptions,
    RegistrySealed,
};

class RpcRegistry {
public:
    [[nodiscard]] std::expected<void, BindError>
    bindRaw(MethodDescriptor descriptor, MethodOptions options, RawHandler handler);

    template <class Params, class Result, class Handler>
    [[nodiscard]] std::expected<void, BindError>
    bind(MethodDescriptor descriptor, MethodOptions options, Handler&& handler);

    template <class Params, class Handler>
    [[nodiscard]] std::expected<void, BindError>
    bindNotification(MethodDescriptor descriptor, MethodOptions options, Handler&& handler);

    void seal();
    [[nodiscard]] bool sealed() const noexcept;
    [[nodiscard]] Json describeMethods() const;

    // 只由 dispatcher 使用。seal() 后返回的 entry 地址在 registry 生命周期内稳定。
    [[nodiscard]] const MethodEntry* find(std::string_view method) const noexcept;
};

} // namespace mcdk::bridge
```

### 13.3 强类型绑定行为

强类型 adapter 使用 `nlohmann::json` 的 `from_json`/`to_json` 转换：

- 参数转换失败返回 `INVALID_PARAMS`。
- handler 可以返回 `Result` 或 `std::expected<Result, RpcError>`。
- notification handler 返回 `void` 或 `std::optional<RpcError>`；错误只进入诊断日志。
- handler 抛出的 `std::exception` 统一转成 `INTERNAL_ERROR`，异常不得越过 worker 边界。
- typed binding 不自动生成 JSON Schema。C++23 没有足够的结构反射，schema 由调用方显式提供或置为 `null`。
- dispatcher 在 typed handler 执行前检查 `gameAvailability`。检查失败时 handler 不得被调用。

示例：

```cpp
struct GetPlayerParams {
    std::string playerId;
};

struct PlayerInfo {
    std::string name;
    double      health;
};

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(GetPlayerParams, playerId)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(PlayerInfo, name, health)

auto result = registry.bind<GetPlayerParams, PlayerInfo>(
    {
        .name = "game/player/get",
        .paramsSchema = {
            {"type", "object"},
            {"required", {"playerId"}},
            {"properties", {{"playerId", {{"type", "string"}}}}},
        },
        .resultSchema = {{"type", "object"}},
    },
    {
        .modes = static_cast<std::uint8_t>(RpcMode::Request),
        .execution = ExecutionPolicy::GameSerial,
        .gameAvailability = GameAvailability::InWorld,
        .timeout = std::chrono::seconds(10),
        .maxConcurrency = 1,
    },
    [&gameService](const RpcContext& context, const GetPlayerParams& params)
        -> std::expected<PlayerInfo, RpcError> {
        return gameService.getPlayer(context.stopToken, params.playerId);
    }
);

if (!result) {
    throw std::runtime_error("failed to bind game/player/get");
}
```

### 13.4 方法名与注册生命周期

- 方法名必须匹配 `^[a-z][a-z0-9_-]*(/[a-z][a-z0-9_-]*)+$`。
- `mcdk/` 前缀保留给协议核心。
- 项目业务建议使用 `game/`、`logs/`、`ui/` 等稳定命名空间。
- 所有内置方法在网络线程启动前注册，然后调用 `seal()`。
- v1 不支持 sealed registry 的动态增删。需要插件热卸载时，创建新 registry snapshot 并原子替换整个只读表，不在原表上修改。
- `describeMethods()` 必须产生按方法名排序的稳定结果。

推荐底层容器使用带 transparent hash/equality 的 `std::unordered_map<std::string, MethodEntry>`，使 `find(std::string_view)` 不分配临时字符串。注册阶段使用互斥锁；`seal()` 后表只读，dispatch 不再加锁。

### 13.5 Dispatcher 执行流程

```text
socket reader
  -> frame/json/envelope 校验
  -> registry.find(method)
  -> mode、GameAvailability、连接级和方法级限流校验
  -> 按 ExecutionPolicy 投递执行器
  -> handler / typed adapter
  -> result 或 RpcError
  -> 有界发送队列
  -> 单一 socket writer
```

`Inline` 只允许框架内部、保证不阻塞且通常在 1 ms 内完成的方法，例如内存中的 `mcdk/session/get` 快照。任何文件 I/O、Game IPC、窗口操作或等待都必须进入 `Worker` 或 `GameSerial`。

`GameStateSnapshot` 必须以锁保护的短临界区或 immutable snapshot 原子发布。网络 dispatcher 只读取 snapshot，不在检查期间调用游戏代码。`GameAvailabilityGuard` 是构造游戏可用性错误的唯一入口，registry 和 `GameService` 不得分别维护不同判定规则。

## 14. 与现有代码的集成方案

### 14.1 模块边界

建议新增：

```text
tools/mcdk/include/host_bridge.hpp
tools/mcdk/include/rpc_registry.hpp
tools/mcdk/src/host_bridge.cpp
tools/mcdk/src/rpc_registry.cpp
```

- `host_bridge` 负责环境配置、TCP、握手、重连、frame、pending request 和生命周期通知。
- `rpc_registry` 只负责方法描述、绑定、查找和 typed adapter，不直接依赖 socket。
- 游戏查询和命令抽成 service 层，由 Host Bridge 注册；MCP handler 也调用同一 service，避免在 `game_process.cpp` 再复制一套 lambda。

### 14.2 启动流程

建议顺序：

1. 在 `startGame()` 中解析世界源目录和运行时目录，构造 `GameLaunchContext`。
2. `launchGameExe()` 检测 `MCDEV_HOST_PORT`。启用 Host Bridge 时，即使 MCP 和热更新关闭，也要启用内部 Game IPC。
3. Game IPC 先绑定随机端口。
4. 注册 RPC 方法并 `seal()` registry。
5. 调用 `CreateProcessW`，取得 Minecraft PID。
6. 构造完整会话快照并启动 Host Bridge 连接线程。
7. 游戏调试 MOD 连接并通过 `ping` 后，更新为 `game_ready` 并发送状态通知。
8. Minecraft 退出后取得 exit code，发送 `exiting`、`exited`，停止连接和执行器，再清理现有 IPC/MCP/热更新资源。

现有 `launchGameExe()` 参数已经不足以表达 `world.sourcePath` 和准确的运行时世界路径。建议显式传入 `GameLaunchContext`，不要在网络模块中根据当前目录重新猜测。

### 14.3 Game IPC 状态

`DebugIPCServer` 当前只能通过 `getClientCount()` 查询连接数。为了避免轮询，建议增加线程安全的连接状态回调：

```cpp
using ClientCountChangedHandler = std::function<void(std::size_t)>;
void setClientCountChangedHandler(ClientCountChangedHandler handler);
```

连接数从 0 变为 1 时进入 `game_ready`，断开到 0 时进入 `game_unavailable`。该状态只表达“是否在存档内”，不再向 Host 暴露客户端线程和服务端线程的内部初始化差异。

建议由一个线程安全的 `GameStateSnapshot` 保存 PID、调试能力状态和 IPC client count。dispatcher 和 `GameAvailabilityGuard` 读取同一份 snapshot，统一构造第 9.6 节的两个错误，禁止各 handler 继续返回当前不稳定的英文文本。

Game IPC 转发仍可能在前置检查后断开。现有 `requestJson()` 的 `No IPC client connected`、`Failed to send IPC JSON request` 和旧 `sendMessage()` 返回 `false` 都映射为 `GAME_WORLD_NOT_READY`。`IPC JSON request timed out` 映射为通用 `HANDLER_TIMEOUT`。

### 14.4 MCP 关系

MCP 继续面向 Agent，Host Bridge 面向 IDE 生命周期和应用 UI。二者可以同时启用。公共业务操作应形成以下依赖方向：

```text
MCP transport ---------+
                       +--> GameService --> DebugIPCServer / LogBuffer / Window API
Host Bridge + registry +
```

transport 层禁止相互调用，例如 Host Bridge 不应通过 HTTP 调用本进程内的 MCP Server。

## 15. 双向验证要求

协议实现合并前，MCDK 和 Host 两端必须使用独立测试替身通过以下用例。

### 15.1 Frame codec

- 使用第 5.2 节的 57 字节测试向量验证大端长度。
- header 按 1/2/1 字节拆分输入仍能解析。
- 两个 frame 一次性输入能解析为两个消息。
- payload 包含中文和换行时，长度按 UTF-8 字节计算。
- 0 长度、16 MiB + 1、非法 UTF-8 和非法 JSON 均关闭连接。

### 15.2 Host 测试

- 先监听，再带环境变量启动 fake MCDK。
- 验证 token、版本、PID、世界路径和连接代次。
- 错误 token 返回 `AUTH_FAILED` 并关闭。
- 同一 `session.id` 的高代次连接替换低代次连接。
- 收到通知不发送响应。
- 对两个并发请求返回乱序响应，MCDK 仍能按 ID 关联。

### 15.3 MCDK 测试

- 未设置端口时完全不启动 bridge。
- Host 晚于 MCDK 就绪时按退避策略连接成功。
- 初始化前收到业务请求，返回 `NOT_INITIALIZED` 或直接关闭。
- Host 断开后重连，`session.id` 不变且 generation 增加。
- 断线时 pending 请求失败且不重放。
- Minecraft 子进程环境不包含 Host port/token。
- 未开启调试能力时，初始化快照包含 `debugCapabilityEnabled=false`，所有 `debug_enabled`/`in_world` 方法返回 `DEBUG_CAPABILITY_DISABLED`。
- Minecraft 已启动但 `getClientCount()==0` 时，游戏转发请求返回 `GAME_WORLD_NOT_READY`，并包含第 9.6 节规定的全部状态字段。
- 同一状态下发送游戏转发通知时，不执行 handler、不返回响应，并产生结构化诊断。
- Minecraft 退出时发送实际 exit code，bridge 线程可在超时内 join。

### 15.4 Registry 测试

- 非法名称、重复名称、seal 后注册均失败。
- raw 和 typed handler 均可调用。
- typed 参数错误映射为 `INVALID_PARAMS`。
- handler 异常映射为 `INTERNAL_ERROR`。
- notification 永不产生响应。
- `DebugEnabled` 和 `InWorld` 都能在状态不足时快速失败，且 handler 未被调用。
- 未开启调试能力返回 `DEBUG_CAPABILITY_DISABLED`。
- `getClientCount()==0` 返回 `GAME_WORLD_NOT_READY` 和完整状态数据。
- 游戏可用性检查失败时，raw 和 typed handler 的调用次数都保持为 0。
- `maxConcurrency` 和连接级上限返回 `SERVER_BUSY`。
- `GameSerial` handler 不并发执行。
- `describeMethods()` 排序和 schema 输出稳定。

### 15.5 端到端测试

复用或扩展现有 `tests/ipc_json_test.cpp`：Host 发起 `game/ping`，MCDK 通过 fake Game IPC 请求，再把结果返回 Host。测试至少覆盖成功、游戏 IPC 未连接、游戏 IPC 超时、Host 中途断线四条路径。

## 16. 版本兼容规则

- JSON-RPC envelope 版本固定为 `2.0`。
- `protocolVersion` 是 MCDK Host Bridge 应用协议版本，v1 为整数 `1`。
- 同一版本只能新增可选字段和新方法，不能改变已有字段类型、错误语义或 frame 格式。
- 接收方必须忽略未知 object 字段，但不得忽略未知 method。
- 删除字段、改变必填性、改变分帧或认证方式必须提升协议主版本。

## 17. v1 实现验收标准

v1 完成的最低标准：

1. Host 监听并通过 `MCDEV_HOST_PORT`、`MCDEV_HOST_TOKEN` 启动 MCDK。
2. MCDK 主动连接并完成带 PID、Game IPC、项目和世界信息的双向握手及 Host 侧令牌认证。
3. `mcdk/ping`、`mcdk/session/get`、`mcdk/methods/list` 可用。
4. 至少一个通知型游戏命令和一个带返回值的游戏查询通过 registry 注册并完成端到端调用。
5. 游戏就绪、断开、退出状态可观察。
6. Host reload 后 MCDK 能重连，且不重放未完成操作。
7. 第 15 节双方契约测试通过。
