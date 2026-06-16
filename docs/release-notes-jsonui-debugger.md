# vX.Y.Z 更新日志：JSON UI 实时调试工具

本版本重点加入并完善了 `jsonui_debugger`：一个面向 AI 和开发者的 Minecraft JSON UI 运行时洞察工具。它通过单一 MCP 工具入口接收命令字符串，读取当前游戏内原生 JSON UI 的 screen、节点、布局、可见性、子树和搜索结果，帮助 AI 在不盲猜路径、不递归爆量、不污染上下文的前提下分析 UI。

## 更新亮点

- 新增单入口工具 `jsonui_debugger(cmd)`，通过 `/help` 按需查看详细命令，避免 MCP 工具列表过度膨胀。
- 支持 screen 列表、节点探测、直接子节点枚举、浅层树读取、HTML-like 布局表达、SVG 布局图和节点搜索。
- 所有树读取命令都带有深度、节点数和扫描上限，默认避免大规模递归读取导致卡顿或上下文爆炸。
- `/overview` 现在是推荐入口：未知 `component_path` 时先用它获取候选根路径和下一步建议。
- 对没有继承常规基类画布的 Mod UI，新增显式 `/overview --nud` fallback，可通过网易官方 UI Debugger 的 `UIDebuggerNotifyEvent` 树发现裸根节点。
- 明确禁止通过 `ScreenNode`、`GetTopUINode()`、`component_path`、回调字段或缓存字段推断根节点；这些属于 Python 用户态封装，不代表 C++ 原生 UI 树语义。
- `/children`、`/tree`、`/find` 等命令现在能安全处理 `get_children_name_from_parent` 返回 `None` 的情况，不再因为 `"/"` 或空路径不可枚举而报错。
- `/render` 的 SVG 输出改为安全文本模式；即使传入历史参数 `--unsafe-svg-image`，也不会再返回 MCP `image/svg+xml` 内容，避免部分客户端把 SVG 图片混入后续模型上下文导致 API 请求失败。

## 命令总览

### `/help`

查看命令清单或某个命令的详细说明。

```text
jsonui_debugger("/help")
jsonui_debugger("/help overview")
jsonui_debugger("/help render")
```

### `/screens`

返回当前原生 JSON UI screen 状态。

```text
jsonui_debugger("/screens")
```

返回内容包括：

- `top_screen`：当前顶层 screen 短名
- `top_screen_fullname`：当前顶层 screen 完整名
- `screens`：当前 screen 完整名列表
- `ui_size`：JSON UI 逻辑尺寸
- `window_size`：客户端窗口尺寸

### `/overview`

推荐的第一步命令。用于不知道 UI 根路径时快速建立上下文。

```text
jsonui_debugger("/overview --screen=top --child-limit=12")
jsonui_debugger("/overview --screen=all --child-limit=12")
jsonui_debugger("/overview --screen=KID_ULTRAX_GAMESET.main --child-limit=12")
```

参数：

- `--screen=top|all|<screen>`：选择当前顶层、全部 screen 或指定 screen。
- `--child-limit=N`：限制每个候选根返回的直接子节点数量。
- `--nud`：显式允许使用网易官方 UI Debugger 树 fallback。仅当普通原生候选根未命中当前 top screen 时使用。

输出重点：

- `roots`：已验证可读的候选根路径
- `suggested_root`：建议后续使用的根路径
- `suggested_next`：推荐下一步 `/html`、`/find`、`/children` 命令
- `nud_enabled`：本次是否允许 NUD fallback
- `root_discovery`：若启用并触发 NUD，则包含官方树候选和验证结果

注意：默认 `/overview` 不会启用 NUD，也不会从 Python 业务对象中推断根路径。对裸根 Mod UI，可显式使用：

```text
jsonui_debugger("/overview --screen=top --child-limit=12 --nud")
```

### `/probe`

检查某个节点路径是否可读，并返回基础信息。

```text
jsonui_debugger("/probe <screen> <path>")
```

示例：

```text
jsonui_debugger("/probe KID_ULTRAX_GAMESET.main /panel")
```

返回内容包括节点类型、可见性、尺寸、位置、全局位置和直接子节点数量。

### `/children`

只列出某个节点的直接子节点，不递归。

```text
jsonui_debugger("/children <screen> <path> [--detail] [--limit=50]")
```

参数：

- `--detail`：为每个子节点附带类型、可见性、尺寸等基础信息。
- `--limit=N`：限制返回数量，默认 50，最大 200。

当底层 API 返回 `None` 时，命令会返回：

```json
{
  "children_count": null,
  "children": [],
  "note": "get_children_name_from_parent returned None; this path is not an enumerable native JSON UI parent."
}
```

这表示该路径不是可枚举的 native JSON UI parent，而不是工具崩溃。

### `/node`

读取单个节点。

```text
jsonui_debugger("/node <screen> <path> [--fields=basic,layout,text,container]")
```

参数：

- `basic`：基础信息，始终返回。
- `layout`：布局表达式、锚点、裁剪等信息。
- `text`：文本类节点的文字、颜色、对齐、阴影等信息。
- `container`：保留字段，用于后续容器类信息扩展。

示例：

```text
jsonui_debugger("/node hud.hud_screen /.../root_panel --fields=basic,layout")
```

### `/tree`

读取有边界的浅层 UI 树。

```text
jsonui_debugger("/tree <screen> <path> [--depth=2] [--max-nodes=80] [--visible-only]")
```

参数：

- `--depth=N`：最大深度，默认 2，最大 8。
- `--max-nodes=N`：最大返回节点数，默认 80，最大 500。
- `--visible-only`：只返回可见节点；隐藏父节点下的可见子节点会被提升保留。

返回内容包含：

- `tree`：结构化节点树
- `truncated`：是否被截断
- `truncated_reason`：截断原因，如 `max_depth`、`max_nodes`、`scan_limit`
- `scanned_nodes` / `returned_nodes`：扫描和返回数量

### `/html`

返回由 Minecraft 运行时布局数据派生的 HTML-like 伪表达，方便 AI 快速理解层级和布局。

```text
jsonui_debugger("/html <screen> <path> [--depth=2] [--max-nodes=80] [--visible-only] [--html-only]")
```

参数：

- `--depth=N`：最大深度，默认 2，最大 6。
- `--max-nodes=N`：最大节点数，默认 80，最大 300。
- `--visible-only`：只保留可见节点。
- `--html-only`：只返回 `html`、`html_note` 和 `summary`，省略完整树，推荐默认使用。

说明：

- `data.html` 不是 JSON UI 源码还原。
- 它不是浏览器可精确渲染的 HTML/CSS。
- 它是从当前游戏运行时布局、锚点、尺寸、位置等数据转换出的阅读辅助表达。

推荐：

```text
jsonui_debugger("/html KID_ULTRAX_GAMESET.main /panel --depth=2 --max-nodes=80 --visible-only --html-only")
```

### `/render`

生成 SVG 布局图，用矩形可视化运行时节点位置和尺寸，主要用于人工检查重叠、尺寸异常、隐藏节点和大致布局关系。

```text
jsonui_debugger("/render <screen> <path> [--depth=2] [--max-nodes=80] [--visible-only] [--label=name|type|path-tail|none] [--legend=false] [--image] [--out=<absolute.svg>] [--unsafe-svg-image]")
```

参数：

- `--depth=N`：最大深度，默认 2，最大 6。
- `--max-nodes=N`：最大节点数，默认 80，最大 300。
- `--visible-only`：只渲染可见节点。
- `--label=name|type|path-tail|none`：控制矩形标签，默认 `name`。
- `--legend=false`：隐藏图例。
- `--image`：返回紧凑文本摘要，不携带 SVG 大字符串。
- `--out=<absolute.svg>`：把 SVG 写入绝对路径，并在工具返回中只保留路径和摘要。
- `--unsafe-svg-image`：历史兼容参数。当前服务端会降级为纯文本摘要，并设置 `unsafe_svg_image_disabled: true`，不会返回 MCP `image/svg+xml` 图片内容。

推荐写出文件：

```text
jsonui_debugger("/render KID_ULTRAX_GAMESET.main /panel --depth=3 --max-nodes=120 --visible-only --label=path-tail --out=D:/Zero123/CPP/CMAKE/MCDevTool/temps/jsonui/panel.svg")
```

安全说明：

- SVG 不是游戏截图，不包含真实贴图。
- AI 默认应优先使用 `/overview`、`/html --html-only`、`/find` 和结构化 JSON。
- 需要用户视觉检查时，推荐使用 `--out=<absolute.svg>`，而不是把 SVG 图片内容直接塞回 MCP 响应。

### `/find`

在有边界的子树中搜索节点。

```text
jsonui_debugger("/find <screen> <path> <query> [--type=Button] [--match=name] [--depth=5] [--limit=30] [--max-nodes=300] [--visible-only]")
```

参数：

- `<query>`：搜索关键字。
- `--type=<Type>`：按原生控件类型过滤，如 `Button`、`Panel`、`Label`。
- `--match=name|path|both`：匹配节点名、完整路径或两者，默认 `name`。
- `--depth=N`：最大深度，默认 5，最大 8。
- `--limit=N`：最大返回匹配数，默认 30，最大 100。
- `--max-nodes=N`：最大扫描节点数，默认 300，最大 1000。
- `--visible-only`：只搜索可见节点。

示例：

```text
jsonui_debugger("/find hud.hud_screen /.../root_panel reset --match=name --type=Button --limit=5")
```

## Mod UI 裸根节点发现

部分 Mod UI 没有挂在常见的原版 HUD 根路径下，而是作为独立 screen 的裸根节点存在。此时：

```text
jsonui_debugger("/children KID_ULTRAX_GAMESET.main /")
```

可能返回 `children_count: null`，因为 `"/"` 不是可枚举的 native parent。

本版本提供显式 NUD fallback：

```text
jsonui_debugger("/overview --screen=top --child-limit=12 --nud")
```

它会短暂启用网易官方 UI Debugger，调用 `gui.nud_get_control_tree("/")`，从 `UIDebuggerNotifyEvent` 返回的 screen 树中提取顶层控件名，例如 `/panel`，再用普通 runtime API 验证该路径是否可枚举、可见、有布局数据。验证成功后，后续 `/children`、`/tree`、`/html`、`/find` 都继续走普通 runtime API。

重要边界：

- 根节点名称由用户 UI 定义决定，不能硬编码为 `/panel`。
- `/panel` 只是当前测试样例中官方 NUD 树返回的顶层控件名。
- 不使用 `ScreenNode`、`GetTopUINode()`、`component_path` 或 Python 对象字段作为根发现依据。

## 建议工作流

1. 先查看 screen：

```text
jsonui_debugger("/screens")
```

2. 不知道根路径时先 overview：

```text
jsonui_debugger("/overview --screen=top --child-limit=12")
```

3. 如果当前是裸根 Mod UI，并且默认 overview 没有候选根，再显式启用 NUD：

```text
jsonui_debugger("/overview --screen=top --child-limit=12 --nud")
```

4. 拿到 `suggested_root` 后，用 HTML-like 输出快速读布局：

```text
jsonui_debugger("/html <screen> <suggested_root> --depth=2 --max-nodes=80 --visible-only --html-only")
```

5. 需要定位按钮或文本时使用搜索：

```text
jsonui_debugger("/find <screen> <suggested_root> close --match=name --limit=20")
```

6. 需要人工视觉检查时写出 SVG：

```text
jsonui_debugger("/render <screen> <suggested_root> --depth=3 --max-nodes=120 --visible-only --label=path-tail --out=D:/path/to/layout.svg")
```

## 兼容性和安全性

- 树类命令默认浅层读取，并带最大节点数保护。
- `/render --unsafe-svg-image` 保留兼容但不会再返回 MCP SVG image content。
- NUD fallback 仅在显式 `--nud` 时触发，且会在操作后清理选择状态、隐藏 bounds 并恢复开关。
- `jsonui_debugger` 主要用于读取和分析运行时 UI；除 NUD 短事务和 SVG 写文件外，默认保持只读。

## 验证情况

已在运行中的 Mod UI 场景验证：

- 当前 top screen 为 `KID_ULTRAX_GAMESET.main`。
- 默认 `/overview --screen=top` 不启用 NUD，不伪造根节点。
- `/overview --screen=top --nud` 可从官方 NUD 树动态发现 `/panel`，并通过普通 runtime API 验证。
- `/children KID_ULTRAX_GAMESET.main /panel` 和 `/html KID_ULTRAX_GAMESET.main /panel --html-only` 正常工作。
- `/children KID_ULTRAX_GAMESET.main /` 安全返回 `children_count: null`，不会报错。
- 最新错误日志为空。
