# 网易 UI Debugger / Safaia 能力

这组能力偏编辑器和选择器，可以用于未来可视化 UI 编辑器，但不适合作为普通 UI 自动化测试或 JSON UI 开发反馈的默认路径。

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

## 注意

- 这些接口受 `mobile_logger.OPEN_SAFAIA_TEST_LOGGER` 开关影响；开关未启用时 wrapper 会直接返回 `False`。
- `nud_*` 接口和 Safaia RPC 更偏编辑器协议，可能通过事件返回数据。
- 如果只是为了列节点、取布局表达式、判断按钮是否可见，优先使用 [runtime-jsonui-api.md](runtime-jsonui-api.md) 的原生 JSON UI 接口。

