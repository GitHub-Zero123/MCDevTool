# 网易 UI Debugger / Safaia 能力

这组能力偏编辑器和选择器，可以用于未来可视化 UI 编辑器，但不适合作为普通 UI 自动化测试或 JSON UI 开发反馈的默认路径。

当前策略：`jsonui_debugger` 的常规读取仍优先使用 [runtime-jsonui-api.md](runtime-jsonui-api.md) 中的只读接口。网易 UI Debugger 会改变用户点击和持续输入状态，因此只允许在显式请求时作为短事务 fallback 使用：当 `/overview --nud` 无法在常规根路径中发现当前 top screen 的根节点时，临时启用调试器，调用 `gui.nud_get_control_tree("/")`，从 `UIDebuggerNotifyEvent` 返回的 screen 树中提取顶层控件名，再立刻清理选择/描边并恢复开关状态。

源码参考：

- `temps/modsdk/minecraft/gui.py`
- `temps/modsdk/safaia/editor_safaia_client/uidebugger/rpc.py`

## 重要接口

| 接口 | 作用 |
| --- | --- |
| `gui.set_netease_ui_debugger_enable(True)` | 启用网易内置 UI 调试 / 编辑器选择模式 |
| `gui.get_netease_ui_debugger_enable()` | 查询是否启用 |
| `gui.nud_set_selected_controls(json_paths)` | 设置选中的控件路径 |
| `gui.nud_get_controls_data(json_paths)` | 获取指定控件数据 |
| `gui.nud_get_control_tree(control_path)` | 获取指定控件树 |
| `gui.nud_set_bounds_visible(visible)` | 设置控件边框显示 |
| `gui.nud_update_bounds_color(updatesJson)` | 更新边框颜色 |

实测注意：

- `gui.nud_get_control_tree("/")` 不通过函数返回值返回树，而是触发 `UIDebuggerNotifyEvent`，事件参数的 `data` 字段是 JSON 字符串。
- 该 JSON 的典型结构是 `{"success": true, "data": {"path": "/", "data": {"type": "screen", "name": "...", "controls": [...]}}}`。
- `nud_*` 查询路径是官方调试器全局路径，会把 screen 根节点也包含在路径中。例如 runtime API 里 `screen_name="hud.hud_screen"`、`component_path="/variables_button_mappings_and_controls/main/KID_ULTRAX_HUD_main"`，对应的 NUD 路径是 `/hud_screen/variables_button_mappings_and_controls/main/KID_ULTRAX_HUD_main`。
- `gui.nud_get_control_tree("/")` 可以看到 HUD 下挂载的 ModSDK 自定义 UI；若要单独查询某个 Mod UI 子树，必须传入包含 screen 根的 NUD 路径。直接把 runtime `component_path` 传给 NUD 可能返回空结果。
- 这只说明 NUD 查询 API 能读到对应子树，不等同于游戏内编辑/选择模式的鼠标点击命中一定能选中该控件。点击选择还受输入模式、HUD 注入层级、控件是否吞输入、当前 top screen 等因素影响。
- 对没有继承常规基类画布的 Mod UI，`controls` 中可能直接出现裸根节点，例如 `/panel`。普通 `gui.get_children_name_from_parent(screen, "/")` 仍可能返回 `None`，不能替代官方调试器树。
- 拿到候选根后，应再用普通 runtime API 验证该路径可枚举、可见、有布局数据；后续 `/tree`、`/html`、`/find` 继续走普通 runtime API。
- 不应通过 `ScreenNode`、`GetTopUINode()`、`component_path`、回调字段或缓存字段兜底推断根节点。这些属于 Python 业务侧面向对象封装，用户态可以改写或伪造，不能代表 C++ 底层 UI 树语义。

## 行为

- `gui.set_netease_ui_debugger_enable(True)` 会让游戏进入 UI 调试/选择模式。
- 开启后点击 UI 会触发选中、描边或检查器相关行为，交互语义会变化。
- 开启期间会影响用户持续输入状态和鼠标/触控行为，不适合长时间保持开启。
- 这对未来“可视化 UI 编辑器”很有用，但不应该作为普通 UI 自动化测试的默认路径。

启用：

```python
import gui

gui.set_netease_ui_debugger_enable(True)
```

关闭并清理选择态：

```python
import gui
import json

gui.nud_set_selected_controls(json.dumps([]))
gui.nud_set_bounds_visible(False)
gui.set_netease_ui_debugger_enable(False)
```

查询状态：

```python
import gui

enabled = gui.get_netease_ui_debugger_enable()
```

## 如果未来封装成 MCP 功能

如果后续确实需要使用 `nud_*` 能力，必须做成短事务，而不是把 debugger 长期开启。

推荐模式：

```python
import gui
import json

old_enabled = gui.get_netease_ui_debugger_enable()

try:
    gui.set_netease_ui_debugger_enable(True)

    # 只执行一次明确操作，例如：
    # gui.nud_get_controls_data(json.dumps(paths))
    # gui.nud_get_control_tree(path)
    # gui.nud_set_selected_controls(json.dumps(paths))
    # gui.nud_set_bounds_visible(True)

finally:
    gui.nud_set_selected_controls(json.dumps([]))
    gui.nud_set_bounds_visible(False)
    gui.set_netease_ui_debugger_enable(old_enabled if old_enabled else False)
```

封装要求：

- 命令执行前临时启用。
- 获取/设置数据后立即恢复原状态。
- 必须 `try/finally` 清理选择态和描边。
- 不允许提供“持续开启 debugger”的默认命令。
- 如果命令失败，也必须恢复点击/输入语义。

如果未来加入 `jsonui_debugger`，建议放在单独命名空间，例如：

```text
/editor get-control-tree <path>
/editor get-controls-data <json_paths>
/editor select <json_paths> --flash
```

并在 `/help editor` 中明确标注“会短暂改变游戏输入状态”。

## 注意

- 这些接口受 `mobile_logger.OPEN_SAFAIA_TEST_LOGGER` 开关影响；开关未启用时 wrapper 会直接返回 `False`。
- `nud_*` 接口和 Safaia RPC 更偏编辑器协议，可能通过事件返回数据。
- 如果只是为了列节点、取布局表达式、判断按钮是否可见，优先使用 [runtime-jsonui-api.md](runtime-jsonui-api.md) 的原生 JSON UI 接口。
- 目前没有证据表明这部分是 JSON UI 实时理解闭环所必需；可先不做，等真正需要可视化选中/描边时再加入。
