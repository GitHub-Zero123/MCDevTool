# 13 · ABI 与事件登记表

上级索引：[README.md](README.md)

## 1. 制度（规范）

> **每新增一个 ABI 函数、一个接口表、或一个事件，必须在本文件登记。缺登记的 PR 由 CI 拒绝合并。**

同样，**每弃用一项也必须在此更新状态**，禁止直接删行——ABI 只增不改（[02-abi-contract.md](02-abi-contract.md) §5.9），登记表也一样，历史必须留痕，否则几年后没人说得清某个字段是什么时候、为什么消失的。

### 1.1 真源与校验方向

| 角色 | 位置 |
| --- | --- |
| **真源** | `sdk/plugin-sdk/include/mcdk/plugin/abi/**` 头文件中的 `@name` / `@since` / `@deprecated` 注释 |
| **人读视图** | 本文件 |

CI **双向**校验（见 [09-compatibility.md](09-compatibility.md) §5）：

- 头文件中有 `@name` 而本表无对应行 → 失败；
- 本表有行而头文件无对应 `@name` → 失败；
- `@since` / `@deprecated` 与本表不一致 → 失败。

双向是关键。只查一个方向的话，删了头里的注释或删了表里的行都能蒙混过关，登记表会缓慢腐烂成一份没人信的文档。

### 1.2 新增一项的完整流程

```text
1. 在对应的 abi/iface/*.h 追加字段（只能加在表尾）
2. 写 @name / @since 注释，@since 填即将发布的 minor
3. 递增 MCDK_ABI_VERSION_MINOR（若本轮尚未递增）   ← 见 02 §8
4. 在本文件登记
5. 在 05-interfaces.md 或 04-events.md 补语义说明
6. 在 examples/00-abi-conformance 补覆盖用例      ← 见 09 §2
7. 宿主 shim 实现，必须经过 host::guard           ← 见 02 §4.3
```

第 6 步同样由 CI 检查：登记表中状态为"可用"的项，若一致性套件未覆盖，则失败。

### 1.3 状态取值

| 状态 | 含义 |
| --- | --- |
| 计划 | 已登记、尚未实现。不得出现在已发布的 ABI 头中 |
| 可用 | 已实现、已被一致性套件覆盖 |
| 已弃用 | 保留可用，但有替代者。注明替代项 |

## 2. 导出符号与入口

| 名称 | @since | 状态 | 说明 |
| --- | --- | --- | --- |
| `mcdk_plugin_entry` | 1.0 | 计划 | 插件唯一导出符号，见 [03](03-abi-reference.md) §2 |
| `mcdk_host_info::get_interface` | 1.0 | 计划 | 接口查询入口，见 [03](03-abi-reference.md) §3 |

## 3. 接口表登记

### 3.1 `mcdk.core/1`

| 字段 | @since | 状态 | 说明 |
| --- | --- | --- | --- |
| `get_last_error` | 1.0 | 计划 | 取当前线程错误槽 |
| `get_host_version` | 1.0 | 计划 | 宿主版本串 |
| `get_stage` | 1.0 | 计划 | 当前生命周期阶段 |
| `get_config` | 1.0 | 计划 | 该条声明的 config JSON 文本；未设置时为 `"null"` |

### 3.2 `mcdk.console/1`

| 字段 | @since | 状态 | 说明 |
| --- | --- | --- | --- |
| `log` | 1.0 | 计划 | 按级别输出，线程安全 |
| `log_colored` | 1.0 | 计划 | 按颜色输出，线程安全 |

### 3.3 `mcdk.info/1`

| 字段 | @since | 状态 | 说明 |
| --- | --- | --- | --- |
| `get_session` | 1.0 | 计划 | 会话信息快照：路径、端口、pid |

### 3.4 `mcdk.game/1`

| 字段 | @since | 状态 | 说明 |
| --- | --- | --- | --- |
| `execute_python` | 1.0 | 计划 | 阻塞执行，禁止在 SYNC 回调中调用 |
| `capture_window` | 1.0 | 计划 | 返回宿主持有的图像句柄 |
| `image_get_info` | 1.0 | 计划 | 查询尺寸与字节数 |
| `image_copy` | 1.0 | 计划 | 拷入调用方缓冲 |
| `image_release` | 1.0 | 计划 | 释放图像句柄 |

### 3.5 `mcdk.log/1`

| 字段 | @since | 状态 | 说明 |
| --- | --- | --- | --- |
| `query` | 1.0 | 计划 | 回调式枚举，持锁期间调用 sink |
| `count` | 1.0 | 计划 | 指定通道的条目数 |

### 3.6 `mcdk.mcp/1`

| 字段 | @since | 状态 | 说明 |
| --- | --- | --- | --- |
| `add_tool` | 1.0 | 计划 | 仅 REGISTER 阶段可用；参数是带 `struct_size` 的 `mcdk_mcp_tool_desc`，不是位置参数 |
| `list_tools` | 1.0 | 计划 | 已注册工具清单 |

### 3.7 `mcdk.events/1`

| 字段 | @since | 状态 | 说明 |
| --- | --- | --- | --- |
| `resolve` | 1.0 | 计划 | 事件名 → 运行期 id，未知返回 0 |
| `subscribe` | 1.0 | 计划 | 返回退订 token |
| `unsubscribe` | 1.0 | 计划 | — |
| `emit` | 1.0 | 计划 | 插件自定义事件 |
| `post_main` | 1.0 | 计划 | 投递到主线程 |

v1 合计 **21 个 ABI 函数**。

## 4. 事件登记

### 4.1 v1

| 事件名 | @since | 状态 | payload | 可否决 |
| --- | --- | --- | --- | :-: |
| `mcdk.mcp.register.before` | 1.0 | 计划 | `mcdk_ev_mcp_register` | 否 |
| `mcdk.mcp.register.finish` | 1.0 | 计划 | `mcdk_ev_mcp_register` | 否 |
| `mcdk.game.launch.before` | 1.0 | 计划 | `mcdk_ev_game_launch_before` | 是 |
| `mcdk.game.launch.finish` | 1.0 | 计划 | `mcdk_ev_game_launch_finish` | 否 |
| `mcdk.game.exit` | 1.0 | 计划 | `mcdk_ev_game_exit` | 否 |
| `mcdk.log.line` | 1.0 | 计划 | `mcdk_ev_log_line` | 是 |
| `mcdk.log.error` | 1.0 | 计划 | `mcdk_ev_log_line` | 是 |
| `mcdk.ipc.client.connected` | 1.0 | 计划 | `mcdk_ev_ipc_client` | 否 |
| `mcdk.ipc.client.disconnected` | 1.0 | 计划 | `mcdk_ev_ipc_client` | 否 |

v1 合计 **9 个事件、6 个 payload 结构体**。

### 4.2 已设计未实现

这些事件的语义已在 [04-events.md](04-events.md) §4 留档，但 **payload 结构体在实现前不得定义**——按 [02-abi-contract.md](02-abi-contract.md) §5.9，一旦定义就只能追加不能改，过早固化没有实现依据的布局是最典型的历史包袱。

| 事件名 | 阻塞在 |
| --- | --- |
| `mcdk.rpc.register.before` / `.finish` | `mcdk.rpc` 未开放 |
| `mcdk.config.resolve.before` / `.finish` | `mcdk.config` 未开放 |
| `mcdk.pack.link.before` / `.finish` | `mcdk.pack` 未开放 |
| `mcdk.world.deploy.before` / `.finish` | 同上 |
| `mcdk.hotreload.file_changed` | `mcdk.hotreload` 未开放 |
| `mcdk.hotreload.trigger.before` / `.finish` | 同上 |
| `mcdk.mcp.tool_call.before` / `.finish` | 待访问控制需求明确 |
| `mcdk.host_bridge.connected` | 待用例明确 |

## 5. 枚举登记

枚举常量值永久冻结（[02-abi-contract.md](02-abi-contract.md) §5.10），新增只能追加。

| 枚举 | @since | 已分配范围 | 下一个可用值 |
| --- | --- | --- | --- |
| `mcdk_status` | 1.0 | 0, -1..-9, -100, -101 | -10（通用段）、-102（屏障段） |
| `mcdk_stage` | 1.0 | 0..4 | 5 |
| `mcdk_log_level` | 1.0 | 0..4 | 5 |
| `mcdk_color` | 1.0 | 0..10 | 11 |
| `mcdk_dispatch_mode` | 1.0 | 0..2 | 3 |
| `mcdk_event_result` | 1.0 | 0..2 | 3 |
| `mcdk_side` | 1.0 | 0..1 | 2 |
| `mcdk_log_channel` | 1.0 | 0..1 | 2 |
| `mcdk_log_order` | 1.0 | 0..1 | 2 |
| `mcdk_image_format` | 1.0 | 0 | 1 |
| `mcdk_mcp_annotation` | 1.0 | 位 0..3 | 位 4 |

"下一个可用值"一列存在的意义是：新增常量时直接取用并更新该列，避免两个并行分支各自选了同一个数值、合并后静默冲突。

## 6. 版本历史

| ABI 版本 | 日期 | 变更 |
| --- | --- | --- |
| 1.0 | 未发布 | 初版：21 个 ABI 函数、9 个事件、6 张接口表 |
