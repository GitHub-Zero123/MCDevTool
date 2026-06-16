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
/help overview
/help children
/help node
/help tree
/help html
/help render
/help find
/help probe
```

建议命令：

| 命令 | 用途 |
| --- | --- |
| `/screens` | 返回当前 screen 栈和 UI 逻辑尺寸 |
| `/overview [--screen=top|all|<screen>] [--child-limit=12]` | 自动探测当前 screen 的建议根路径和直接子节点摘要 |
| `/probe <screen> <path>` | 安全检查路径是否存在、类型、直接子节点数量 |
| `/children <screen> <path> [--detail] [--limit=50]` | 返回直接子节点 |
| `/node <screen> <path> [--fields=basic,layout,text,container]` | 返回单节点布局和内容信息 |
| `/tree <screen> <path> [--depth=2] [--max-nodes=80] [--visible-only]` | 安全浅层树 |
| `/html <screen> <path> [--depth=2] [--max-nodes=80] [--visible-only] [--html-only]` | 返回由 MC 实时布局数据派生的 HTML 伪表达，仅用于参考布局 |
| `/render <screen> <path> [--depth=2] [--max-nodes=80] [--visible-only] [--label=name] [--out=<absolute.svg>]` | 返回或写出 SVG 布局图，主要给用户视觉检查 |
| `/find <screen> <path> <query> [--type=Button] [--match=name] [--depth=5] [--limit=30]` | 按名称/路径/类型搜索，默认只匹配节点名 |

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

### `/overview`

```text
jsonui_debugger("/overview --screen=top --child-limit=12")
```

这是推荐的第一步命令。它会返回当前 screen、可用的候选根路径、直接子节点摘要和 `suggested_next`，避免 AI 在不知道 `component_path` 时盲试 `/`。

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
jsonui_debugger("/html hud.hud_screen /.../root_panel --depth=2 --max-nodes=80 --visible-only --html-only")
```

返回给 AI 阅读的 HTML 伪表达。实现上仍由 Py 侧读取结构化树数据，C++ MCP 层在解析 JSON 后追加 `data.html`，避免把表达转换逻辑放进游戏执行环境。

注意：`data.html` 是由 Minecraft 当前运行时渲染/布局数据转换来的参考表达，只用于快速理解节点层级、类型、锚点、尺寸和位置。它不是 JSON UI 源码还原，也不是浏览器可精确渲染的 HTML/CSS。

默认调试时建议加 `--html-only`，只返回 `html`、`html_note` 和 `summary`，避免完整 `tree` 占用过多上下文。需要详细结构化树时再去掉该选项。

### `/render`

```text
jsonui_debugger("/render hud.hud_screen /.../root_panel --depth=2 --max-nodes=80 --visible-only --label=name")
```

返回 SVG 布局图字符串，用不同颜色和透明度展示运行时节点矩形。它主要给用户看，用于快速发现重叠、0 尺寸、隐藏节点和大致布局关系。它不是游戏截图，不包含真实贴图，也不是 JSON UI 源码还原。

如果希望用户稳定查看 SVG，推荐写出到完整路径：

```text
jsonui_debugger("/render hud.hud_screen /.../root_panel --depth=2 --max-nodes=80 --visible-only --label=path-tail --out=D:/Zero123/CPP/CMAKE/MCDevTool/temps/jsonui/root_panel.svg")
```

`--out` 要求绝对路径且扩展名为 `.svg`，会自动创建父目录。此模式下 tool 返回紧凑文本、`svg_written` 和 `svg_path`，不会把 SVG 大字符串或 MCP image content 混入后续模型上下文。用户/客户端可以直接打开该文件，AI 也可以按需读取 SVG 文本。

如果希望压缩文本上下文，可以追加 `--image`：

```text
jsonui_debugger("/render hud.hud_screen /.../root_panel --depth=2 --max-nodes=80 --visible-only --label=path-tail --image")
```

此时 MCP 返回仍是文本 content，但文本摘要会移除 `data.svg` 大字符串，只保留 `summary` 和说明。不要默认返回 `type=image`、`mimeType=image/svg+xml` 的 MCP content：部分 Agent 客户端会把工具图片结果混入后续模型上下文，而上游模型/网关未必接受 SVG 图片块，可能导致后续聊天请求失败。

历史版本中存在实验开关：

```text
jsonui_debugger("/render hud.hud_screen /.../root_panel --depth=2 --max-nodes=80 --visible-only --label=path-tail --unsafe-svg-image")
```

`--unsafe-svg-image` 现在仅为兼容旧命令而保留；服务端会降级为纯文本摘要，并设置 `unsafe_svg_image_disabled: true`，不会再额外返回 `type=image`、`mimeType=image/svg+xml` 的 content。需要给用户视觉检查时，请使用 `--out=<absolute.svg>` 写出文件。

可用选项：

- `--label=name|type|path-tail|none`：控制矩形标签。`path-tail` 适合大树里辨认相近节点。
- `--legend=false`：隐藏底部图例，适合空间很小的 UI。
- `--out=<absolute.svg>`：把 SVG 写到绝对路径，并在 tool 返回中只保留路径和摘要。
- `--image`：移除文本摘要里的 SVG 大字符串，只保留紧凑摘要，避免上下文膨胀。
- `--unsafe-svg-image`：兼容旧命令；服务端仍只返回紧凑文本，不再返回 MCP `image/svg+xml` image content。

AI 默认仍应优先使用 `/overview`、`/html --html-only`、`/find` 和结构化 JSON；只有当客户端/模型具备足够图像理解能力时，才把 `/render` 输出当作辅助输入。

### `/find`

```text
jsonui_debugger("/find hud.hud_screen /.../root_panel reset --match=name --type=Button --limit=5")
```

默认 `--match=name` 只匹配节点名，避免父路径中包含 `button`、`panel` 等词时污染搜索结果。需要路径搜索时可显式使用 `--match=path` 或 `--match=both`。

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
