# 单工具设计：`jsonui_debugger(cmd)`

为了减少 MCP tool 数量和上下文污染，UI 实时分析能力只暴露为一个工具：

```text
jsonui_debugger(cmd: string)
```

工具描述建议：

```text
Analyze native Minecraft JSON UI runtime state. Use jsonui_debugger("/help") to list commands. Supports screen listing, node lookup, shallow tree inspection, layout snapshots, and HTML-like pseudo output. The tool is read-only by default and applies depth/node limits to avoid large UI dumps.
```

这个设计的好处：

- MCP tool 列表只有一个入口，不会把大量细碎工具描述塞进模型上下文。
- 详细命令通过 `/help` 按需获取。
- C++ bridge / MCP schema 后续不用频繁变更。
- 安全策略统一放在 `jsonui_debugger` 内部执行。

## 命令清单

默认 `/help` 只给短清单。详细说明通过 `/help <command>` 获取。

```text
/help
/help screens
/help children
/help node
/help tree
/help html
/help find
/help probe
```

建议命令：

| 命令 | 用途 |
| --- | --- |
| `/screens` | 返回当前 screen 栈和 UI 逻辑尺寸 |
| `/probe <screen> <path>` | 安全检查路径是否存在、类型、直接子节点数量 |
| `/children <screen> <path> [--detail] [--limit=50]` | 返回直接子节点 |
| `/node <screen> <path> [--fields=basic,layout,text,container]` | 返回单节点布局和内容信息 |
| `/tree <screen> <path> [--depth=2] [--max-nodes=80] [--visible-only]` | 安全浅层树 |
| `/html <screen> <path> [--depth=2] [--max-nodes=80] [--visible-only]` | 返回 HTML 伪表达 |
| `/find <screen> <path> <query> [--type=Button] [--depth=5] [--limit=30]` | 按名称/路径/类型搜索 |

## 返回格式

所有命令都返回 JSON 字符串，外层统一：

```json
{
  "ok": true,
  "cmd": "/screens",
  "data": {}
}
```

错误：

```json
{
  "ok": false,
  "cmd": "/node ...",
  "error": {
    "message": "path not found",
    "code": "PATH_NOT_FOUND"
  }
}
```

树类命令必须包含：

```json
{
  "truncated": false,
  "truncated_reason": null,
  "visited_nodes": 12,
  "returned_nodes": 12,
  "max_depth": 2,
  "max_nodes": 80
}
```

## 命令示例

### `/screens`

```text
jsonui_debugger("/screens")
```

返回：

```json
{
  "ok": true,
  "cmd": "/screens",
  "data": {
    "top_screen": "pause_screen",
    "screens": ["hud.hud_screen", "pause.pause_screen"],
    "ui_size": [487.0, 272.0],
    "window_size": [1947.0, 1089.0]
  }
}
```

### `/children`

```text
jsonui_debugger("/children hud.hud_screen /.../root_panel --limit=50")
```

返回直接子节点，不递归：

```json
{
  "ok": true,
  "cmd": "/children",
  "data": {
    "screen": "hud.hud_screen",
    "path": "/.../root_panel",
    "children_count": 31,
    "children": [
      {"name": "left_helpers", "path": "/.../root_panel/left_helpers"}
    ],
    "truncated": false
  }
}
```

### `/node`

```text
jsonui_debugger("/node hud.hud_screen /.../left_helpers --fields=basic,layout")
```

返回单节点结构：

```json
{
  "ok": true,
  "cmd": "/node",
  "data": {
    "name": "left_helpers",
    "type": "Panel",
    "visible": true,
    "computed": {
      "size": [487.0, 0.0],
      "position": [10.0, 272.0],
      "global_position": [10.0, 272.0]
    },
    "layout": {
      "anchor_from": "bottom_left",
      "anchor_to": "bottom_left"
    }
  }
}
```

### `/tree`

内部只允许逐层调用 `get_children_name_from_parent`，禁止直接裸调递归接口。

```text
jsonui_debugger("/tree hud.hud_screen /.../root_panel --depth=2 --max-nodes=80 --visible-only")
```

### `/html`

```text
jsonui_debugger("/html hud.hud_screen /.../root_panel --depth=2 --max-nodes=80")
```

返回给 AI 阅读的 HTML 伪表达。实现上仍由 Py 侧读取结构化树数据，C++ MCP 层在解析 JSON 后追加 `data.html`，避免把表达转换逻辑放进游戏执行环境。

## IPC 脏数据处理

当前已有 `execute_code` 能力可以直接执行 Python 并拿返回值。封装版 `jsonui_debugger` 可以走同一 IPC 通道：

```text
jsonui_debugger(cmd)
-> C++ 生成安全 Python 代码
-> IPC execute_code
-> 返回文本可能包含前缀/日志等脏数据
-> C++ 从返回文本中定位第一个 JSON 对象再解析
```

解析策略：

1. 优先读取 `return_value` 字段。
2. 如果只拿到文本，找到第一个 `{` 或 `[`。
3. 从该位置开始做括号配平，截出第一个完整 JSON。
4. 解析失败时返回原始文本摘要，不直接丢弃。

这部分可以放在 C++ 工具层统一处理，避免每个命令重复写解析逻辑。

## 安全默认值

| 项 | 默认 |
| --- | --- |
| `/tree --depth` | `2` |
| `/tree --max-nodes` | `80` |
| `/find --depth` | `5` |
| `/find --limit` | `30` |
| `/children --limit` | `50` |
| 隐藏节点 | 默认摘要，不展开隐藏子树 |

禁止：

- 无限制递归。
- 大根节点上直接调用 `get_all_children_path_from_parent`。
- 大根节点上批量调用 `get_property_bag_value`。
