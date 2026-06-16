# 建议封装的 MCP Tool

这些 tool 面向“AI 写 JSON UI -> 游戏验证 -> AI 修正”的开发闭环。

## `jsonui_list_screens`

返回当前 screen 栈和 UI 逻辑尺寸。

```json
{
  "ok": true,
  "top_screen": "pause_screen",
  "screens": ["hud.hud_screen", "pause.pause_screen"],
  "ui_size": [487, 272],
  "window_size": [1947, 1089]
}
```

## `jsonui_get_children`

只返回直接子节点，默认不递归。

参数：

```json
{
  "screen": "hud.hud_screen",
  "path": "/.../root_panel",
  "limit": 50,
  "include_detail": false
}
```

返回：

```json
{
  "ok": true,
  "children_count": 31,
  "children": [
    {"name": "left_helpers", "path": "/.../root_panel/left_helpers"}
  ],
  "truncated": false
}
```

## `jsonui_get_node_layout`

读取单个节点的结构化布局信息。

参数：

```json
{
  "screen": "hud.hud_screen",
  "path": "/.../left_helpers",
  "fields": ["basic", "layout", "text", "container"]
}
```

返回：

```json
{
  "ok": true,
  "node": {
    "name": "left_helpers",
    "type": "Panel",
    "visible": true,
    "computed": {
      "size": [487, 0],
      "position": [10, 272],
      "global_position": [10, 272]
    },
    "layout": {
      "anchor_from": "bottom_left",
      "anchor_to": "bottom_left"
    }
  }
}
```

## `jsonui_get_tree_shallow`

分层读取树，默认最多 2 层。内部应该只使用 `get_children_name_from_parent` 逐层展开，不直接调用递归接口。

参数：

```json
{
  "screen": "hud.hud_screen",
  "root": "/.../root_panel",
  "max_depth": 2,
  "max_nodes": 80,
  "visible_only": true,
  "nonzero_size_only": false,
  "fields": ["type", "visible", "computed", "layout_summary"]
}
```

返回必须包含：

- `truncated`
- `truncated_reason`
- `visited_nodes`
- `returned_nodes`

## `jsonui_find_nodes`

按名称、路径、类型搜索节点。内部仍应逐层展开，不能直接递归全量。

参数：

```json
{
  "screen": "hud.hud_screen",
  "root": "/.../root_panel",
  "query": "button",
  "match": "fuzzy",
  "type": null,
  "max_depth": 5,
  "max_nodes": 300,
  "limit": 30
}
```

## `jsonui_to_html_pseudo`

把结构化节点树转成 HTML 伪表达，不直接查询游戏。它应该消费 `jsonui_get_tree_shallow` 的结果，避免查询和表达转换耦合。

参数：

```json
{
  "source": "tree_result_id_or_inline_tree",
  "mode": "readable",
  "include_data_attrs": true,
  "include_hidden": false
}
```

输出：

```html
<div data-type="Panel" data-path="/root_panel" style="width:100%;height:100%;">
  <div data-type="Panel" data-path="/root_panel/left_helpers"
       data-anchor-from="bottom_left"
       data-anchor-to="bottom_left"
       style="position:absolute;left:10px;bottom:0;width:100%;height:0;">
  </div>
</div>
```

## `jsonui_write_probe_snapshot`

大范围排查时写文件，不把完整树直接塞回模型上下文。

返回：

```json
{
  "ok": true,
  "file": "temps/ui-snapshots/hud-root-20260616.json",
  "summary": {
    "visited_nodes": 1200,
    "returned_nodes": 1200,
    "hidden_nodes": 740
  }
}
```

