# Minecraft Runtime Agent 设计规范

## 定位

Minecraft Runtime Agent 是 MCDK 面向 AI 的游戏内自动执行层。它不把 LLM 当作低频按键器，而是把 LLM 放在规划、观察、纠错和任务编排的位置；游戏内 runtime 负责 tick 级动作闭环、状态机推进、失败恢复和输入清理。

最终形态应当是：

```text
Codex / MCP Client
  负责目标拆解、策略选择、日志分析、失败后换方案

MCDK MCP Server
  负责结构化 Tool 暴露、权限收敛、任务轮询、日志和状态摘要

Minecraft Runtime Agent
  负责游戏内观察、移动、寻路、挖掘、放置、攻击、UI 点击等 tick 级控制
```

LLM 不直接控制每 tick 的前进、跳跃、转向、持续挖掘、攻击冷却等细节。这些控制必须在游戏侧由确定性的状态机维护。

## 核心原则

1. **高层目标输入**：MCP 只接收 `move_to`、`mine_block`、`place_block`、`attack_entity` 这类目标命令，不暴露 `press_forward`、`set_yaw`、`continue_destroy_block` 等低层接口。
2. **游戏侧闭环**：每个长动作都必须由游戏侧 runtime 每 tick 推进，自己完成转向、移动、跳跃、寻路、重试、超时、清理和结果验证。
3. **受控行为通道并发**：任务不是全局单状态，而是按行为通道管理。同一通道只保留一个当前状态，新任务覆盖旧任务；不同通道只有在资源不冲突或冲突可被 runtime 稳定仲裁时才允许并存。
4. **异步任务模型**：所有耗时动作都返回 `task_id`，由 MCP 轮询状态。不要让一次 Tool 调用阻塞到动作完成。
5. **结构化观察**：Agent 观察结果必须是结构化数据，截图只作为补充，不作为主状态来源。
6. **动作返回增量世界状态**：第一次决策通常需要 `agent_observe` 建立基线；后续动作结果必须携带执行期间的关键事件、最终状态和必要的局部观察，避免 LLM 每一步都重新全量查询。
7. **最小暴露面**：MCP 顶层 Tool 数量保持少量，动作类型通过参数区分，避免 AI 上下文被大量细碎工具污染。
8. **尊重 MC 沙盒**：MCDK Agent 运行在 Minecraft / 网易 ModSDK 已有沙盒内，安全越界问题由引擎环境处理；设计重点放在动作可用性、可观测性、状态恢复和输入清理。
9. **可取消与可恢复**：任何会锁定输入的任务都必须支持取消，并在完成、失败、超时、断连时释放所有输入锁。

## MCP Tool 设计

顶层只暴露以下工具。

### `agent_observe`

读取当前游戏状态，作为 LLM 决策入口。

参数：

```json
{
  "radius": 16,
  "include_blocks": false,
  "include_entities": true,
  "include_inventory": true,
  "include_ui": false
}
```

返回：

```json
{
  "ok": true,
  "player": {
    "pos": [10.5, 64.0, -20.5],
    "rot": {"pitch": 0.0, "yaw": 90.0},
    "dimension": "overworld",
    "game_mode": "creative",
    "health": 20,
    "flying": false
  },
  "held_item": {"slot": 0, "name": "minecraft:diamond_pickaxe", "count": 1},
  "entities": [],
  "blocks_summary": {},
  "top_screen": "hud.hud_screen",
  "tasks": []
}
```

观察数据必须短小、稳定、可解析。大范围方块扫描必须显式启用，并设置半径上限。

`agent_observe` 用于建立决策基线，或在任务结果信息不足、世界状态被外部因素改变、任务链断裂时重新同步状态。常规任务链不应每一步都全量 observe。

### `agent_start_task`

启动一个游戏内异步任务。

参数：

```json
{
  "task": "move_to",
  "args": {
    "x": 20,
    "y": 64,
    "z": -12
  }
}
```

返回：

```json
{
  "ok": true,
  "task_id": "task_12",
  "task": "move_to",
  "status": "running"
}
```

如果新任务所属通道已有运行中任务，旧任务会被自动取消并返回在 `replaced` 字段中。不同通道的任务不自动取消，但必须经过资源仲裁；不能稳定仲裁的组合应被拒绝、暂停低优先级任务，或由任务类型显式降级。

### `agent_start_tasks`

批量启动多个任务，用于支持不具备并行 tool call 能力的 LLM Agent。该工具在同一 IPC 请求内按顺序安装多个行为通道，保证“移动 + 战斗守护”“移动 + 环境扫描”等稳定组合能原子化启动。

参数：

```json
{
  "tasks": [
    {
      "task": "move_to",
      "args": {"x": 20, "y": 64, "z": -12}
    },
    {
      "task": "guard_combat",
      "args": {"radius": 8, "hostile_only": true}
    }
  ]
}
```

返回：

```json
{
  "ok": true,
  "started": [
    {"task_id": "task_12", "task": "move_to", "channel": "movement", "status": "running"},
    {"task_id": "task_13", "task": "guard_combat", "channel": "combat", "status": "running"}
  ],
  "replaced": []
}
```

批量启动不是并行线程。它只是一次性更新多个行为通道；实际执行仍由游戏侧 runtime 每 tick 协调。

批量启动必须支持冲突报告：

```json
{
  "ok": false,
  "started": [
    {"task_id": "task_12", "task": "move_to", "channel": "movement", "status": "running"}
  ],
  "failed": [
    {
      "task": "mine_block",
      "error": {
        "code": "RESOURCE_CONFLICT",
        "message": "mine_block cannot start while movement is active unless pause_movement=true"
      }
    }
  ],
  "replaced": []
}
```

对于会改变世界或锁定输入的任务，批量启动不应默认事务回滚已经成功启动的其他通道。调用方可以显式传入 `atomic=true`，要求任一任务失败时取消本批次已经启动的任务。

允许的任务类型：

| 任务 | 说明 |
| --- | --- |
| `move_to` | 地面导航到指定坐标 |
| `fly_to` | 飞行到指定坐标，需要创造模式或已允许飞行 |
| `follow_entity` | 跟随实体，适合动态目标 |
| `guard_combat` | 战斗守护，移动或执行其他任务期间自动处理敌意实体 |
| `mine_block` | 破坏指定方块 |
| `place_block` | 在指定位置放置方块 |
| `use_item_on` | 对指定位置使用手持物品 |
| `attack_entity` | 靠近并攻击指定实体 |
| `click_ui` | 点击指定 UI 控件 |
| `wait` | 非阻塞等待 |
| `run_command` | 执行游戏内开发命令 |

任务类型映射到行为通道：

| 通道 | 同通道覆盖任务 | 可并存说明 |
| --- | --- | --- |
| `movement` | `move_to`、`fly_to`、`follow_entity` | 可与 `combat_guard`、`observe` 并存；不与 `interaction` 同时推进 |
| `combat` | `attack_entity` | 精确攻击任务，占用玩家视角和攻击输入；通常覆盖或暂停 `combat_guard` |
| `combat_guard` | `guard_combat` | 守护任务，可与移动并存，但只能短暂接管视角/攻击 |
| `interaction` | `mine_block`、`place_block`、`use_item_on` | 需要稳定视角；执行期间暂停 movement 和 combat_guard |
| `ui` | `click_ui` | 与世界交互互斥；执行期间暂停 movement、combat_guard、interaction |
| `observe` | 长扫描任务 | 不应锁定输入 |
| `command` | `run_command` | 游戏内命令通道 |

通道并发不是无条件并发。任务必须声明自己会占用哪些资源：

| 资源 | 说明 |
| --- | --- |
| `look` | 玩家视角 |
| `move` | 移动输入 |
| `jump` | 跳跃输入 |
| `vertical_move` | 飞行升降输入 |
| `attack` | 攻击输入 |
| `use` | 使用/放置输入 |
| `ui_touch` | UI 点击输入 |

Runtime 每 tick 根据资源占用仲裁任务。高优先级任务可以临时抢占资源，但必须在本 tick 或阶段结束后释放。

默认允许的并发组合只包含少数稳定情况：

| 组合 | 策略 |
| --- | --- |
| `movement + combat_guard` | 移动持续推进；发现敌意实体时 combat_guard 临时接管 `look` 和 `attack`，移动保持或短暂停顿 |
| `movement + observe` | observe 不锁输入，可并行采样 |
| `wait + 任意非互斥任务` | wait 只计时，不占输入资源 |
| `command + observe` | 命令执行后由 observe 或任务 delta 同步状态 |

默认拒绝或暂停的组合：

| 组合 | 策略 |
| --- | --- |
| `interaction + movement` | interaction 需要稳定视角和距离；先暂停 movement，完成后可恢复 |
| `interaction + combat_guard` | 挖掘/放置期间不允许守护抢视角；除非生命危险事件触发任务失败并移交 combat |
| `ui + world task` | UI 点击期间暂停世界输入 |
| `combat + combat_guard` | 精确攻击覆盖守护任务 |

这种设计不是为了追求“所有任务都并行”，而是为了支持少数确实有价值、且能稳定仲裁的组合。Minecraft 玩家输入资源高度共享，尤其是视角资源；无限并发会让移动、挖掘和攻击互相破坏，最终降低完成率。

### `agent_get_task`

查询任务状态。

参数：

```json
{
  "task_id": "task_12"
}
```

返回：

```json
{
  "ok": true,
  "task_id": "task_12",
  "task": "move_to",
  "status": "completed",
  "elapsed_ticks": 86,
  "result": {
    "arrived": true,
    "final_pos": [20.2, 64.0, -12.1],
    "distance": 0.25
  },
  "observation_delta": {
    "player": {
      "pos": [20.2, 64.0, -12.1],
      "rot": {"pitch": 2.0, "yaw": 91.5},
      "health": 20,
      "flying": false
    },
    "nearby_entities_changed": false,
    "top_screen": "hud.hud_screen"
  },
  "events": [
    {
      "type": "waypoint_reached",
      "tick": 24,
      "data": {"index": 3, "total": 8}
    }
  ],
  "timeline": [
    "path found: 8 waypoints",
    "waypoint 3/8 reached",
    "arrived"
  ]
}
```

任务结果必须是 LLM 的下一步主要输入。`result` 描述动作目标是否完成，`observation_delta` 描述动作完成后与下一步决策直接相关的状态，`events` 描述执行期间发生的重要事实，`timeline` 提供短文本诊断。

`observation_delta` 不应复制完整 `agent_observe`。它只返回与该任务相关、且下一步决策高概率需要的信息：

| 任务 | 必带增量状态 |
| --- | --- |
| `move_to` / `fly_to` | 最终玩家位置、朝向、生命值、是否仍在飞行、是否卡住过 |
| `mine_block` | 目标方块最终状态、玩家位置、使用工具、耗时 |
| `place_block` | 目标方块最终状态、手持物品/数量变化、玩家位置 |
| `attack_entity` | 目标是否存在、目标血量变化、玩家血量、玩家位置 |
| `click_ui` | 点击坐标、顶层 screen 是否变化、关键控件文本或状态变化 |
| `run_command` | 执行命令数量、命令回显/错误、相关状态变化 |

状态值固定为：

```text
running
completed
failed
timeout
cancelled
```

### `agent_cancel_task`

取消指定任务，并释放输入锁。

参数：

```json
{
  "task_id": "task_12"
}
```

返回：

```json
{
  "ok": true,
  "task_id": "task_12",
  "cancelled": true
}
```

### `agent_get_events`

读取并清空 Agent 事件队列。

返回：

```json
{
  "ok": true,
  "events": [
    {
      "type": "chat_message",
      "timestamp": 1780000000.0,
      "data": {"sender": "Steve", "message": "done"}
    }
  ],
  "dropped": 0
}
```

事件队列用于补充任务结果，例如聊天、死亡、维度切换、UI 打开、目标实体消失等。

正常任务轮询结果已经包含该任务执行期间收集到的相关事件。`agent_get_events` 主要用于读取没有归属到某个任务的全局事件，或在长时间空闲后同步外部变化。

## 游戏侧 Runtime 设计

游戏内 Runtime 由以下模块组成。

### `AgentRuntime`

生命周期入口，由 ClientSystem 创建、启动、tick 更新和销毁。

职责：

- 注册任务类型；
- 维护任务管理器；
- 维护事件队列；
- 处理来自 MCDK IPC 的结构化请求；
- 在断连、退出存档、重载时取消任务并释放输入锁。

### `AgentBridge`

MCDK C++ 侧与游戏内 Python Runtime 的通信桥。

Agent 不维护独立 socket。所有请求统一走 MCDK 已有 Debug IPC，由 C++ MCP Tool 组装结构化 JSON，再通过 IPC 投递到游戏内 Python 包。Python 侧提供一个稳定入口，例如：

```python
def handle_agent_request(payload):
    ...
```

通信路径固定为：

```text
MCP Client
  -> MCDK C++ MCP Tool
  -> DebugIPCServer requestJson / execute_code
  -> 游戏内 Python AgentRuntime.handle_request
  -> AgentTaskManager / AgentObserve
```

请求格式：

```json
{
  "method": "agent.start_task",
  "params": {
    "task": "move_to",
    "args": {"x": 1, "y": 64, "z": 1}
  }
}
```

游戏内 Python 返回：

```json
{
  "ok": true,
  "result": {}
}
```

C++ 侧只负责：

- 参数校验；
- IPC 连接检查；
- 调用游戏内 Python 入口；
- MCP 返回值格式化；
- 超时与错误包装。

Python 侧负责：

- 任务创建与推进；
- 状态缓存；
- 事件收集；
- 输入锁清理；
- 返回结构化结果。

这条链路避免 MCP、IPC、Agent socket 三套连接状态并存，也让 Agent 能直接复用 MCDK 已有的游戏进程生命周期、日志、重载和错误处理能力。

### `AgentTaskManager`

tick 级异步任务管理器，也是行为通道调度器。

职责：

- `start_task(type, args)` 创建任务；
- 按行为通道保存当前任务；
- 同通道新任务覆盖旧任务；
- 根据资源占用决定每 tick 哪些任务推进、暂停或取消；
- 每 tick 调用可推进任务的 `tick()`；
- 收集完成、失败、超时、取消结果；
- 支持 `cancel_task` 和 `cancel_all`；
- 维护简短 timeline；
- 限制同时运行任务数量。

调度器维护两个核心表：

```text
active_channels: channel -> task
resource_owner: resource -> task
```

每个任务声明：

```json
{
  "channel": "movement",
  "resources": ["look", "move", "jump"],
  "priority": 50,
  "can_pause": true,
  "can_resume": true
}
```

每 tick 调度顺序：

1. 清理已完成、失败、超时、取消任务；
2. 处理同通道覆盖产生的取消任务；
3. 按优先级和允许的并发组合排序；
4. 给任务分配本 tick 资源；
5. 资源不足时暂停可暂停任务；
6. 无法暂停且冲突不可仲裁时让新任务启动失败，返回 `RESOURCE_CONFLICT`；
7. 推进获得资源的任务；
8. 收集每个任务产生的 `events` 和 `observation_delta`。

任务暂停不是失败。暂停任务必须收到 `on_pause(reason)`，恢复时收到 `on_resume()`。任何持有输入锁的任务在暂停前必须释放输入锁。

### `MovementController`

低层移动控制器，不暴露给 MCP。

职责：

- 平滑旋转到目标；
- 锁定前进输入；
- 按需跳跃；
- 水中上浮；
- 检测卡住；
- 释放输入锁。

所有使用 `LockInputVector`、`LockVerticalMove`、持续挖掘、持续使用物品的任务都必须在 `cleanup()` 中释放对应状态。

## 任务实现规范

### `move_to`

输入：

```json
{"x": 10, "y": 64, "z": 20, "range": 1.0}
```

行为：

1. 获取玩家当前位置；
2. 请求引擎寻路；
3. 沿 waypoint 移动；
4. 卡住时尝试跳跃或重新寻路；
5. 到达阈值内完成；
6. 超时或路径失败时返回结构化错误。

失败码：

| 错误码 | 说明 |
| --- | --- |
| `PATH_NOT_FOUND` | 无法生成路径 |
| `PATHFIND_TIMEOUT` | 寻路超时 |
| `STUCK` | 长时间无法前进 |
| `NOT_REACHED` | 路径结束但未到达 |
| `INPUT_LOCK_FAILED` | 输入锁异常 |

### `mine_block`

输入：

```json
{"x": 10, "y": 64, "z": 20}
```

行为：

1. 读取目标方块；
2. 如果已为空气，直接完成；
3. 检查距离，必要时先靠近；
4. 转向目标方块中心；
5. `start_destroy_block`；
6. 每 tick `continue_destroy_block`；
7. 根据方块硬度和工具估算超时；
8. 验证目标变为空气。

失败码：

| 错误码 | 说明 |
| --- | --- |
| `BLOCK_NOT_FOUND` | 无法读取方块 |
| `TOO_FAR` | 目标不可交互且无法靠近 |
| `START_DESTROY_FAILED` | 开始破坏失败 |
| `MINING_INTERRUPTED` | 持续破坏中断 |
| `MINING_TIMEOUT` | 超时未破坏 |
| `VERIFY_FAILED` | 验证未通过 |

### `place_block`

输入：

```json
{"x": 10, "y": 64, "z": 20, "item": "minecraft:dirt"}
```

行为：

1. 检查目标位置是否可替换；
2. 检查背包或手持物品；
3. 必要时切换 hotbar；
4. 找到相邻可点击方块面；
5. 转向并执行放置；
6. 验证目标方块。

失败码：

| 错误码 | 说明 |
| --- | --- |
| `BLOCK_EXISTS` | 目标位置不可替换 |
| `ITEM_NOT_FOUND` | 未找到目标物品 |
| `NO_ADJACENT_BLOCK` | 没有可点击的相邻方块 |
| `PLACE_FAILED` | 放置失败 |
| `VERIFY_FAILED` | 验证未通过 |

### `attack_entity`

输入：

```json
{"entity_id": "-123456"}
```

行为：

1. 确认实体存在；
2. 跟随靠近攻击距离；
3. 停止移动并瞄准；
4. 执行攻击；
5. 读取血量、死亡状态或实体是否消失；
6. 返回伤害结果。

攻击任务应区分：

- `completed`：确实执行攻击并完成验证；
- `skipped`：目标消失、已死亡、不可达，不算错误；
- `failed`：任务逻辑异常或输入锁失败。

### `click_ui`

输入：

```json
{"screen": "hud.hud_screen", "path": "/root/panel/button"}
```

行为：

1. 读取控件可见性；
2. 读取全局位置和尺寸；
3. 计算中心点；
4. 分两 tick 发送 DOWN / UP；
5. 返回点击坐标。

UI 结构发现仍由 `jsonui_debugger(cmd)` 负责，`click_ui` 只负责执行点击。

## 观察能力规范

`agent_observe` 默认返回轻量状态，不做大扫描。

默认包含：

- 玩家位置；
- 玩家朝向；
- 当前维度；
- 当前游戏模式；
- 生命值；
- 飞行状态；
- 手持物品；
- 顶层 UI；
- 当前任务列表。

可选包含：

- 附近实体；
- 指定半径方块统计；
- 指定方块搜索；
- 背包列表；
- UI 文本和控件状态。

方块扫描必须限制半径，默认最大 16，硬上限 32。更大的扫描必须拆成明确任务，避免卡顿。

## 运行环境

MCDK Agent 面向本地开发者调试环境，运行在 Minecraft / 网易 ModSDK 已有沙盒内。引擎和 ModSDK 已经处理了 Mod 侧能力边界，Agent 设计不需要再额外解释或维护一套安全越界限制。

因此，游戏内命令、破坏、放置、攻击、给予物品、修改游戏模式等都视为开发期可用能力。设计重点不是限制这些能力，而是确保它们：

- 以结构化任务形式执行；
- 有清晰状态和结果；
- 可取消、可恢复；
- 不残留输入锁；
- 能返回执行期间发生的关键事件和增量状态。

### 输入锁清理

以下情况必须调用 `cancel_all()` 并释放输入锁：

- MCP 客户端断开；
- 游戏退出存档；
- 热重载；
- `reload_game`；
- 任务超时；
- 任务异常；
- 同通道任务被新任务覆盖；
- 任务因资源仲裁被暂停；
- 任务因资源冲突启动失败。

## 与现有 MCDK 能力的关系

### 与 `execute_code`

`execute_code` 是开发调试入口，不是 Agent 默认动作入口。

Agent 不应依赖 AI 动态生成任意 Python 代码来完成常规动作。常规动作必须通过结构化 MCP Tool 调用已注册任务，原因是结构化任务更稳定、可轮询、可取消、可恢复，并且能返回执行期间的事件和增量状态。

### 与 `jsonui_debugger`

`jsonui_debugger(cmd)` 负责 UI 观察和路径分析。

Agent 的 `click_ui` 负责执行点击。

推荐流程：

```text
jsonui_debugger("/overview")
jsonui_debugger("/find ...")
agent_start_task({"task":"click_ui", ...})
agent_get_task(...)
```

### 与自动热更新

Agent Runtime 必须能承受 Python/UI/Shader/Material 热更新期间的短暂状态变化。

UI 热更新、游戏重载、Addon 重载前应取消所有运行中 Agent 任务。重载后由 MCP 重新观察状态，再决定是否继续任务。

## 推荐实现顺序

### 第一阶段：稳定闭环

必须一次做到可用：

1. `agent_observe`
2. `agent_start_task`
3. `agent_get_task`
4. `agent_cancel_task`
5. `wait`
6. `move_to`
7. `click_ui`

验收标准：

- 可启动任务；
- 可轮询完成；
- 可取消任务；
- 移动任务结束后不会锁住玩家输入；
- UI 点击跨 tick 触发；
- `jsonui_debugger` 能提供路径，Agent 能点击该路径。

### 第二阶段：世界交互

1. `mine_block`
2. `place_block`
3. `use_item_on`
4. `find_blocks`
5. `get_inventory`
6. `select_slot`

验收标准：

- 能自动靠近目标；
- 能验证方块变化；
- 背包物品不足时结构化失败；
- 任务失败后输入状态干净。

### 第三阶段：实体交互

1. `get_nearby_entities`
2. `follow_entity`
3. `attack_entity`
4. `shoot_at_entity`

验收标准：

- 目标消失时返回 `skipped`；
- 目标不可达时返回明确失败；
- 攻击后返回血量变化或实体消失状态。

## MCP 使用模式

Codex 的典型循环：

```text
1. agent_observe 建立初始状态基线
2. 判断目标与当前状态差距
3. agent_start_task
4. agent_get_task 轮询
5. 从 result / observation_delta / events / timeline 获取执行期间发生的事实
6. 直接基于任务返回继续下一步决策
7. 只有信息不足、状态被外部因素打断、任务链断裂时才重新 agent_observe
```

示例：

```json
{
  "tool": "agent_start_task",
  "arguments": {
    "task": "move_to",
    "args": {"x": 100, "y": 64, "z": 100}
  }
}
```

```json
{
  "tool": "agent_get_task",
  "arguments": {
    "task_id": "task_1"
  }
}
```

LLM 不需要也不应该知道每 tick 怎么转向、怎么跳、怎么继续挖掘。它需要的是初始状态、每个任务返回的增量状态、执行期间事件、失败原因，以及下一步该换什么目标。

## 对 `temps/modsdk_3.9/agent` 原型的处理

该目录中的实现可以作为参考，但不能原样作为最终架构。

可复用的部分：

- tick 驱动任务管理模型；
- `move_to`、`mine_block`、`place_block`、`attack_entity` 的状态机思路；
- 环境观察 API 的范围；
- `click_ui` 跨 tick DOWN/UP 思路；
- 任务 `task_id` 和 poll 模型。

需要重写或修正的部分：

- 独立 socket 通信模型；
- 任意脚本执行作为默认入口；
- 将 `run_commands` 作为默认脚本入口，而不是结构化开发任务；
- 不稳定的反编译残留代码；
- 方块可替换判断；
- Python 沙箱模块限制；
- 错误码和结果结构；
- 输入锁清理的一致性。

最终实现要以 MCDK MCP 为入口，以结构化任务为核心，以游戏侧闭环为基础。
