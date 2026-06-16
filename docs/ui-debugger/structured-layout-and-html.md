# 结构化布局与 HTML 伪表达

AI 更熟悉 HTML/CSS，但我们的目标产物是网易 JSON UI。中间表示应同时服务两件事：

- 机器可读：保留 JSON UI 的真实结构和布局表达式。
- AI 可读：提供接近 HTML/CSS 的伪 DOM，帮助模型理解布局。

## 结构化 UI 布局信息

推荐节点结构：

```json
{
  "screen": "hud.hud_screen",
  "path": "/root/panel/button",
  "name": "button",
  "type": "Button",
  "visible": true,
  "layout": {
    "size_x": {"fit": false, "followType": "parent", "absoluteValue": 0, "relativeValue": 1},
    "size_y": {"fit": false, "followType": "none", "absoluteValue": 40, "relativeValue": 0},
    "position_x": {"followType": "parent", "absoluteValue": 0, "relativeValue": 0.5},
    "position_y": {"followType": "none", "absoluteValue": 12, "relativeValue": 0},
    "anchor_from": "top_middle",
    "anchor_to": "top_middle"
  },
  "computed": {
    "size": [160, 40],
    "position": [243.5, 12],
    "global_position": [243.5, 12]
  },
  "children_count": 3
}
```

字段分层：

- `identity`：`screen/path/name/type`
- `computed`：游戏实际计算结果，如尺寸、坐标、可见性
- `layout`：JSON UI 布局表达式，如 `followType/relativeValue/absoluteValue/fit`
- `content`：文本、图片、item、model 等内容字段
- `interaction`：按钮、滑块、滚动、输入相关状态
- `children`：仅在明确需要树结构时展开

## HTML 伪表达

示例：

```html
<button
  data-screen="hud.hud_screen"
  data-path="/root/panel/button"
  data-type="Button"
  data-anchor-from="top_middle"
  data-anchor-to="top_middle"
  style="
    position: absolute;
    width: calc(parent.width * 1 + 0px);
    height: 40px;
    left: calc(parent.width * 0.5 + 0px);
    top: 12px;
    transform-origin: top center;
  ">
  <span>按钮文本</span>
</button>
```

转换原则：

| JSON UI 类型 | HTML 伪元素 |
| --- | --- |
| `Panel` / `InputPanel` | `div` |
| `StackPanel` | `div data-layout="stack"` |
| `Label` / `RichText` | `span` 或 `p` |
| `Image` | `img` 或 `div background-image` |
| `Button` | `button` |
| `Toggle` | `input type="checkbox"` 或 `button aria-pressed` |
| `Slider` | `input type="range"` |
| `ScrollView` | `div style="overflow:auto"` |
| `Grid` / `StackGrid` | `div data-layout="grid"` |

## 布局表达式到 CSS 伪表达

`get_size_x/y` 返回：

```json
{"absoluteValue": 20, "relativeValue": 0.5, "followType": "parent", "fit": false}
```

可表达为：

```css
width: calc(parent.width * 0.5 + 20px);
```

常见映射：

| JSON UI 表达 | HTML 伪表达 |
| --- | --- |
| `fit=true` | `width:100%` / `height:100%` |
| `followType=none` | `Npx` |
| `followType=parent` | `calc(parent.axis * r + Npx)` |
| `followType=children` | `fit-content`，并保留注释 |
| `followType=maxChildren` | `max-content`，并保留注释 |
| `followType=maxSibling` | `data-follow="maxSibling"` |

锚点不能简单等同于 CSS。必须保留原始字段：

```html
data-anchor-from="bottom_left"
data-anchor-to="bottom_left"
```

再尽量推导 CSS：

```css
left: 10px;
bottom: 0;
transform-origin: bottom left;
```

## 隐藏节点策略

运行时树里会有大量隐藏、0 尺寸、模板或布局辅助节点。给 AI 的默认视图建议：

- 可见节点完整输出。
- 隐藏节点只输出摘要。
- 0 尺寸但可见的节点保留，因为它可能是布局容器。
- 只有用户明确要求时才展开隐藏子树。

