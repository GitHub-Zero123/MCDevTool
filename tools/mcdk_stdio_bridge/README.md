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
- `tools/list` 不依赖游戏是否已启动，直接返回共享工具定义；传了 `--project` 时还会加上项目里插件声明的工具（见下）。
- 不做后台线程、不做周期性重连、不在 MCP 初始化阶段连接游戏。
- `tools/call` 被调用时才触发一次连接/初始化尝试：
  - 成功：转发调用到 `mcdk` 游戏 MCP；
  - 失败：以 tool error 形式返回游戏未启动或 MCP 未配置的说明。
- `mc_profiler` 是一个 `op + args` 工具，不会为每种 profiler 注册独立工具。
- `mc_profiler /help` 和 `/guide` 也先尝试后端，使 MCDK 的首次 profiler op 能惰性探测 Native DLL 并返回真实 capability；后端不可达时才返回共享静态帮助，并将 runtime 标记为 unavailable/unknown。
- 其他 `mc_profiler` op 在后端不可达时只返回 unavailable；bridge 不会本地创建 service、加载 DLL、扫描端口、启动采集或持有任务。

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

下面几例是只用内置工具的最简形态。**项目里的插件要暴露 MCP 工具时，请改按工作区配置**，见[插件声明的工具](#插件声明的工具)。

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

## 插件声明的工具

插件可以在自己的 `plugin.json` 里用 `mcpTools` 声明工具（见 `docs/plugin-system/06-loading.md` §3.3）。这类声明写在磁盘上，所以桥接工具不需要任何东西在运行就能把它们列进 `tools/list` —— 这正是本工具存在的理由的延伸：客户端只在启动时问一次清单，那时游戏和 `mcdk` 都还没起来。

要列出它们，得用 `--project` 告诉桥接工具项目在哪（指向 `.mcdev.json` 所在目录）。**不能靠工作目录**：MCP 客户端拉起子进程时的 cwd 通常是客户端自己的，不是项目目录。

### 用到插件工具时，请按工作区配置桥接工具

**只要项目里的插件要暴露 MCP 工具，就把桥接工具配在工作区级配置里，不要配在客户端的全局配置里。**

工具清单是跟着项目走的：不同项目装不同插件，声明的工具也不同。工作区级配置让每个工作区各自带一份，换项目时自然就对。

```jsonc
// .vscode/mcp.json —— 工作区级，且能展开变量，换工作区不用改
{
    "servers": {
        "minecraft_be_mcdk": {
            "command": "mcdk_stdio_bridge",
            "args": ["--project", "${workspaceFolder}"]
        }
    }
}
```

客户端不支持变量展开时，在工作区配置里写死绝对路径也行——反正那份配置本来就只服务这一个项目：

```jsonc
"args": ["--project", "D:/MyAddon"]
```

### 全局配置：能用，但会错配

放在客户端全局配置里时，一个桥接进程会被多个工作区复用，而它只在启动时被问一次工具清单。`--project` 可以给多次，列出的是各项目声明的并集：

```jsonc
"args": ["--project", "D:/AddonA", "--project", "D:/AddonB"]
```

代价要清楚：

- 并集里会有当前 `mcdk` 实例并没加载的工具，AI 调用它只会得到"工具不存在"——而 AI 在那之前已经按这个工具规划过了；
- 新增项目要手动补一条 `--project`；
- 工具名在多个项目间重复时，先列出的胜出，另一个永远不可见。

所以全局配置适合"只用内置工具"的场景。**一旦涉及插件工具，请改用工作区配置。**

### 其他约束

- 不传 `--project` 时只列内置工具，行为与以前一致。
- 只读清单文本，**不加载任何插件动态库**。
- `enable: false` 的插件不贡献工具；`path` 直指动态库（而非目录）的声明没有清单，也不贡献。
- 工具名与内置工具冲突时，内置的优先，与 `mcdk` 内部注册表的规则一致。
- 调用这类工具仍然要 `mcdk` 在跑。没跑时和其他工具一样返回"游戏未启动"，而不是"工具不存在"——对 AI 来说这两者的区别很大。

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
