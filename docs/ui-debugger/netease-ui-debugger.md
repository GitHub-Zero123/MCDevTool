# 网易 UI Debugger / Safaia 能力

这组能力偏编辑器和选择器，可以用于未来可视化 UI 编辑器，但不适合作为普通 UI 自动化测试或 JSON UI 开发反馈的默认路径。

当前结论：**MVP 阶段不建议把这部分封装进 `jsonui_debugger`**。我们现在的目标是让 AI 读取原生 JSON UI 的实时布局信息，优先使用 [runtime-jsonui-api.md](runtime-jsonui-api.md) 中的只读接口即可。网易 UI Debugger 会改变用户点击和持续输入状态，纳入 MCP 后容易干扰真实操作。

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
