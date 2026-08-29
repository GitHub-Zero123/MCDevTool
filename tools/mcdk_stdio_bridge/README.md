# MCDK Stdio MCP Bridge

`mcdk_stdio_bridge` 是给 VSCode / Agent / IDE Agent 使用的 stdio 形态 MCP `跳板`服务。

## 为什么需要这个工具

`mcdk` 内置的游戏 MCP 服务随游戏进程一起启动，只有通过 `mcdk` 启动游戏且 `mcdev.json` 中启用了 `mcp_server_config.enabled` 后，客户端才能连接到 `http://localhost:19133`。

这对很多 MCP 客户端不友好：

- VSCode / IDE Agent 通常只在启动时连接一次 MCP；
- 游戏尚未启动时，直接连接 `mcdk` 内置 MCP 会失败；
- 部分 Agent 不会主动重试，导致后续即使游戏启动也无法使用 MCP 工具；
- 某些客户端只支持 stdio MCP 配置，不适合直接连 SSE / Streamable HTTP。

`mcdk_stdio_bridge` 解决这个问题：它本身始终以 stdio MCP 方式被客户端启动，只暴露与 `mcdk` 内置 MCP 一致的 tools 列表；真正调用 tool 时才尝试连接游戏 MCP。若游戏 MCP 已可用则转发请求，否则返回明确的“游戏未启动或未启用 MCP”错误。

## 行为说明

- 只提供 stdio MCP 入口，不启动 HTTP/SSE 服务。
- 默认在端口区间 `19133-19142` 内发现正在运行的 MCDK 实例（默认端口 `19133` 落在区间内）。
- 可通过 args 覆盖 host / 端口；端口既接受旧的单值写法，也接受区间写法。
- `tools/list` 不依赖游戏是否已启动，直接返回共享工具定义。
- 不做后台线程、不做周期性重连、不在 MCP 初始化阶段连接游戏。
- `tools/call` 被调用时才触发一次连接/初始化尝试：
  - 成功：转发调用到 `mcdk` 游戏 MCP；
  - 失败：以 tool error 形式返回游戏未启动或 MCP 未配置的说明。
- `mc_profiler` 是一个 `op + args` 工具，不会为每种 profiler 注册独立工具。
- `mc_profiler /help` 和 `/guide` 也先尝试后端，使 MCDK 的首次 profiler op 能惰性探测 Native DLL 并返回真实 capability；后端不可达时才返回共享静态帮助，并将 runtime 标记为 unavailable/unknown。
- 其他 `mc_profiler` op 在后端不可达时只返回 unavailable；bridge 不会本地创建 service、加载 DLL、启动采集或持有任务。

## 实例发现与多开

区间模式下 bridge 不做盲目的端口扫描：

1. 读取系统 TCP 监听表（Windows），只挑出区间内确实有人监听、且本机可经 loopback 连上的端口；若其中存在 `mcdk` 可执行文件持有的端口则优先只保留这些。
2. 对每个候选端口做一次 MCP 握手，并调用 `mcdk_instance_info` 取回实例身份（端口、世界名、Minecraft 进程号、工程目录）。

> MCP 服务跑在 `mcdk` 进程里而不是 Minecraft 进程里；Minecraft 进程内的是另一套调试 IPC。

非 Windows 平台或监听表读取失败时，退化为在区间内逐个探测。

发现结果决定转发目标：

- **发现 1 个实例**：自动选中，调用方无感，与旧版行为一致。
- **发现多个实例**：bridge 不猜，工具调用会返回带候选列表的错误，提示先用 `mcdk_use` 指定端口。
- **选中的实例退出**：只剩一个实例时静默切换过去；仍有多个时重新要求选择。

bridge 额外提供两个自身的工具（不会转发给后端）：

- `mcdk_instances`：列出区间内发现的实例，标出当前选中项。
- `mcdk_use`：传入 `port` 切换目标实例；不传 `port` 则清除选择、回到自动发现。

以固定单值端口启动 bridge 时不做发现，这两个工具也不可用。

## 前置配置

需要在项目的 `mcdev.json` 中启用 `mcdk` 内置 MCP：

```jsonc
{
    "mcp_server_config": {
        "enabled": true,
        "server_ip": "localhost",
        // 多开测试时写成区间，例如 "19133-19142"
        "server_port": 19133
    }
}
```

然后用 `mcdk` 正常启动游戏。桥接工具可以早于游戏启动被 Agent 加载。

## MCP 客户端配置

支持标准 MCP 客户端接入。与主 README 中直接连接 `mcdk` 内置 SSE MCP 的配置不同，本工具是 stdio MCP，因此配置项应使用 `command` / `args`。

### Roo Code MCP Settings

```jsonc
{
    // Roo Code MCP Settings
    "mcpServers": {
        "minecraft_be_mcdk": {
            "command": "mcdk_stdio_bridge",
            "args": []
        }
    }
}
```

如果可执行文件没有加入 `PATH`，请把 `command` 改成构建产物的绝对路径，例如：

```jsonc
{
    // Roo Code MCP Settings
    "mcpServers": {
        "minecraft_be_mcdk": {
            "command": "D:/.../mcdk_stdio_bridge.exe",
            "args": []
        }
    }
}
```

### VSCode（Copilot）`.vscode/mcp.json`

```jsonc
{
    "servers": {
        "minecraft_be_mcdk": {
            "command": "mcdk_stdio_bridge",
            "args": []
        }
    }
}
```

## 自定义端口

不传端口参数时 bridge 在 `19133-19142` 内发现实例，覆盖了默认端口，通常无需配置。

如果 `mcdev.json` 中的 `mcp_server_config.server_port` 落在这个区间之外，才需要同步传给桥接工具。**传单个端口时行为与旧版本完全一致**：锁定该端口、不做发现、不可切换实例。

```jsonc
{
    "mcpServers": {
        "minecraft_be_mcdk": {
            "command": "mcdk_stdio_bridge",
            "args": ["--port", "19134"]
        }
    }
}
```

多开测试时传端口区间，让 bridge 发现区间内的全部实例：

```jsonc
{
    "mcpServers": {
        "minecraft_be_mcdk": {
            "command": "mcdk_stdio_bridge",
            "args": ["--port", "19133-19142"]
        }
    }
}
```

端口参数支持的写法：

| 写法 | 含义 |
| --- | --- |
| `--port 19133` | 单值端口，锁定该端口（旧行为） |
| `--port 19133-19142` | 端口区间 |
| `--port=19133-19142` | 同上，等号形式 |
| `--ports 19133 19142` | 两个独立参数写区间 |
| `--port-range 19133 19142` | 同上 |
| `19133-19142` | 位置参数 |

区间起止写反（如 `19142-19133`）会自动按升序处理；跨度上限 32，超出部分会被截断并在 stderr 给出提示。

也可同时指定 host：

```jsonc
{
    "mcpServers": {
        "minecraft_be_mcdk": {
            "command": "mcdk_stdio_bridge",
            "args": ["--host", "localhost", "--port", "19133-19142"]
        }
    }
}
```

VSCode（Copilot）的 `.vscode/mcp.json` 同理使用 `servers` 作为顶层字段，其余 `command` / `args` 内容保持一致。

## 与直接连接 mcdk MCP 的区别

直接连接 `mcdk` 内置 MCP 适合客户端支持 SSE / Streamable HTTP 且会自动重连的场景；`mcdk_stdio_bridge` 更适合只支持 stdio 或不会主动重试的 Agent 场景。

桥接工具不替代 `mcdk` 内置 MCP，也不会自动启动游戏；它只负责延迟连接与请求转发。当前策略是“调用时连接”，不是“周期性连接”。
