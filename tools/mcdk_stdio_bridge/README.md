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
- 默认转发到 `http://localhost:19133/mcp`，端口对应 `mcdk` 默认 MCP 端口。
- 可通过 args 覆盖 host / port。
- `tools/list` 不依赖游戏是否已启动，直接返回共享工具定义。
- 不做后台线程、不做周期性重连、不在 MCP 初始化阶段连接游戏。
- `tools/call` 被调用时才触发一次连接/初始化尝试：
  - 成功：转发调用到 `mcdk` 游戏 MCP；
  - 失败：以 tool error 形式返回游戏未启动或 MCP 未配置的说明。

## 前置配置

需要在项目的 `mcdev.json` 中启用 `mcdk` 内置 MCP：

```jsonc
{
    "mcp_server_config": {
        "enabled": true,
        "server_ip": "localhost",
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

如果 `mcdev.json` 中的 `mcp_server_config.server_port` 不是默认 `19133`，需要同步传给桥接工具：

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

也可同时指定 host：

```jsonc
{
    "mcpServers": {
        "minecraft_be_mcdk": {
            "command": "mcdk_stdio_bridge",
            "args": ["--host", "localhost", "--port", "19133"]
        }
    }
}
```

VSCode（Copilot）的 `.vscode/mcp.json` 同理使用 `servers` 作为顶层字段，其余 `command` / `args` 内容保持一致。

## 与直接连接 mcdk MCP 的区别

直接连接 `mcdk` 内置 MCP 适合客户端支持 SSE / Streamable HTTP 且会自动重连的场景；`mcdk_stdio_bridge` 更适合只支持 stdio 或不会主动重试的 Agent 场景。

桥接工具不替代 `mcdk` 内置 MCP，也不会自动启动游戏；它只负责延迟连接与请求转发。当前策略是“调用时连接”，不是“周期性连接”。
