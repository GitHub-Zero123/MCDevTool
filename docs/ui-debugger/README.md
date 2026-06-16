# UI Debugger / JSON UI 开发辅助概述

这个目录记录我们为“让 AI 参与网易 JSON UI 开发”整理出的游戏侧能力、运行时探针策略和后续 MCP Tool 设计。

核心目标不是做传统可视化编辑器，而是形成一个稳定闭环：

```text
AI 编写 JSON UI -> 游戏运行验证 -> 读取结构化布局信息 -> 转 HTML 伪表达 -> AI 继续修正 JSON
```

## 阅读顺序

1. [runtime-jsonui-api.md](runtime-jsonui-api.md)
   原生 JSON UI 运行时接口。这里是主线，包含 `screen_name + component_path`、控件树、控件类型、几何结果、布局表达式、文本和图片相关 API。

2. [safety-probing.md](safety-probing.md)
   游戏内探针安全策略。重点记录哪些接口可以安全调用，哪些接口必须限深、限节点数，避免再次因为递归大树或重字段导致游戏崩溃。

3. [structured-layout-and-html.md](structured-layout-and-html.md)
   面向 AI 的结构化布局 JSON 和 HTML 伪表达设计。后续 AI 更适合读这种中间表示，而不是直接猜 JSON UI。

4. [mcp-tools.md](mcp-tools.md)
   建议封装的 MCP Tool：列 screen、列子节点、取单节点布局、浅层树、转 HTML 伪表达等。

5. [netease-ui-debugger.md](netease-ui-debugger.md)
   可选附录。`gui.set_netease_ui_debugger_enable(True)` 和 `nud_*` 偏编辑器/选择器，会影响用户点击和持续输入状态，MVP 阶段不建议纳入默认 MCP 功能。

## 当前结论

- JSON UI 开发反馈只围绕 `minecraft.gui` 原生界面接口展开。
- 关键低风险接口是 `get_all_screen_fullnames`、`get_control_def_type`、`get_children_name_from_parent`、`get_size/get_position/get_global_position/get_visible`、`get_size_x/y`、`get_position_x/y`、`get_anchor_from/to`。
- `get_all_children_path_from_parent`、`recursively_get_children`、`get_property_bag_value` 必须谨慎封装，不能裸调大节点。
- `set_netease_ui_debugger_enable` / `nud_*` 暂不作为 MVP 主线；若未来封装，必须短事务启用、操作后立即禁用并清理状态。
- AI 需要两类输出：结构化 UI 布局 JSON，以及保留 JSON UI 语义的 HTML 伪表达。

## 实测基线

2026-06-16 当前游戏状态曾实测：

```json
{
  "all_screen_fullnames": ["hud.hud_screen", "pause.pause_screen"],
  "top_screen": "pause_screen",
  "client_ui_screen_size": [487.0, 272.0],
  "client_screen_size": [1947.0, 1089.0]
}
```

`client_ui_screen_size` 是布局应使用的 UI 逻辑尺寸；`client_screen_size` 是窗口像素尺寸，不应直接当作 JSON UI 坐标系。
