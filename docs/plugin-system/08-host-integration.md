# 08 · 宿主侧集成与前置重构

上级索引：[README.md](README.md)

## 1. 前置 A：抽出 `RuntimeSession`

`launchGameExe()` 现有 979 行，把 `ipcServer`、`logBuffer`、`errBuffer`、`mcpServer`、5 个 watcher task、`hostBridgeTask`、`profilerRuntime` 全部作为局部变量持有。接口 shim 无法访问这些对象，因而无从实现 `mcdk.game` 等接口。

**必须**先抽出 `RuntimeSession`，持有上述全部运行期对象并提供访问器；`launchGameExe` 改为构造 `RuntimeSession` 并驱动它。接口 shim 一律通过 `host::sessionOf(self)` 取得会话再转发。

不做这一步，后续每新增一个接口都要修改 `launchGameExe`。

### 1.1 分批拆解建议

979 行一次性搬完风险太高。按"谁依赖谁"分三批，每批独立可测、可单独提 PR：

| 批次 | 对象 | 依赖 |
| --- | --- | --- |
| 一 | `logBuffer`、`errBuffer`、`profilerGamePid` | 无 |
| 二 | `ipcServer`、`profilerRuntime`、`mcpServer` | 批一 |
| 三 | 5 个 watcher task、`hostBridgeTask`、`styleProcessor` | 批一、批二、`UserConfig` |

**第一个 PR 只做纯机械替换**：`RuntimeSession` 仅持有对象并提供访问器，不搬任何逻辑，`launchGameExe` 的函数体结构保持原样，局部变量换成 `session.xxx()`。这样 diff 全是改名，review 成本低，也不会掩盖行为变化。

逻辑搬迁（例如把 `mcpServer` 的那一大串 `setXxxHandler` 绑定挪进 `RuntimeSession` 的成员函数）放在第二个 PR，届时已有 `session` 作为落脚点。

完成判据：`launchGameExe` 只剩"构造 session → 配置 → 启动进程 → 等待 → 终结"的骨架，各子系统的装配细节都在 `RuntimeSession` 内部。

## 2. 前置 B：MCP 工具动态注册表

当前 MCP 工具在 `mcp_tool_definitions.cpp` 中静态定义，通过 `MCPServer::setXxxHandler()` 逐个绑定，插件无法插入。

**必须**按 `RpcRegistry` 的形态（含 `seal()`）新建 `McpToolRegistry`，把现有内置工具改为注册进该表。`mcdk.mcp` 接口即是对该表的薄封装。

注册时序：

```text
McpToolRegistry 创建
  → 注册全部内置工具
  → 发射 mcdk.mcp.register.before    ← 插件在此注册
  → registry.seal()
  → 发射 mcdk.mcp.register.finish
  → MCPServer 依据注册表 register_tool
```

`RpcRegistry` 已有 `seal()`，只需在其前后补发 `mcdk.rpc.register.before` / `.finish` 两个事件即可，无需重构。

## 3. 阶段推进点

| 阶段 | 触发位置 |
| --- | --- |
| `REGISTER` | `main.cpp`，`userParseConfig()` 之后、`startGame()` 之前 |
| `CONFIG` | `startGame()` 开头，游戏路径校验之后 |
| `WORLD` | `startGame()` 中 manifest 写盘之前 |
| `RUNTIME` | `launchGameExe()` 中 `RuntimeSession` 构造完成、游戏进程启动之后 |
| `SHUTDOWN` | `launchGameExe()` 返回前，逆序 |

`SHUTDOWN` 之后的插件终结顺序是强规范，见 [03-abi-reference.md](03-abi-reference.md) §5——必须先断开事件派发并排空 in-flight 回调，再调 `on_unload`，顺序反了会把事件打进正在析构的插件对象。

事件与阶段的时序见 [04-events.md](04-events.md) §2。

## 4. 插件加载的接入点

插件声明来自 `.mcdev.json`（见 [06-loading.md](06-loading.md)），因此加载必须发生在配置解析之后：

```text
main.cpp
  userParseConfig()                    ← 解析 .mcdev.json，含 plugins 数组
  PluginHost::loadDeclared(config)     ← 新增：按声明加载、校验、拓扑排序
  PluginHost::advance(STAGE_REGISTER)
  startGame(config)
    PluginHost::advance(STAGE_CONFIG)
    ...
```

`UserConfig` 需新增 `std::vector<PluginDeclaration> plugins` 字段，由 `config.cpp` 解析。**注意**：`.mcdev.json` 的解析发生在插件加载之前，因此插件**无法**影响自己的声明，这是刻意的。

## 5. 事件发射点

| 事件 | 发射位置 |
| --- | --- |
| `mcdk.mcp.register.*` | `McpToolRegistry` 封存前后（见 §2） |
| `mcdk.rpc.register.*` | `RpcRegistry::seal()` 前后 |
| `mcdk.config.resolve.*` | `startGame()` 开头 |
| `mcdk.pack.link.*` | `linkUserConfigModDirs()` 前后 |
| `mcdk.world.deploy.*` | `deployWorldSource()` / `createUserLevel()` 前后 |
| `mcdk.game.launch.*` | `launchGameExe()` 中创建游戏进程前后 |
| `mcdk.game.exit` | 游戏进程监视线程检出退出时 |
| `mcdk.log.line` / `.error` | `LogBuffer` 写入路径 |
| `mcdk.ipc.client.*` | `DebugIPCServer` 客户端连接回调 |
| `mcdk.hotreload.*` | `ConsoleWatcherTask::onFileChanged` / `onHotReloadTriggered` |
| `mcdk.mcp.tool_call.*` | `McpToolRegistry` 分发入口 |
| `mcdk.host_bridge.connected` | `HostBridgeTask` 连接建立回调 |

## 6. shim 层规约

**规范：`components/plugin-host/src/interfaces/` 下的每一个导出函数指针都必须经过 `host::guard`，没有例外。** 见 [02-abi-contract.md](02-abi-contract.md) §4.3，该规则由 CI 静态检查（见 [09-compatibility.md](09-compatibility.md) §4）。

shim 层**禁止**包含业务逻辑，只做三件事：参数转换、调用 `RuntimeSession` 上的现有实现、结果转换。任何新的业务逻辑都应落在 `mcdk_core` / `mcdk_runtime` 中，以便非插件路径复用。
