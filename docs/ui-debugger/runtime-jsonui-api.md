# 原生 JSON UI 运行时 API

这条线是 AI 开发 JSON UI 的主路径。它按 `screen_name + component_path` 读取游戏中真实运行的 JSON UI 控件状态。

源码参考：

- `temps/modsdk/minecraft/gui.py`
- `temps/modsdk/mod/client/ui/screenNode.py`
- `temps/modsdk/mod/client/ui/controls/baseUIControl.py`
- `temps/modsdk/mod/client/ui/controls/UIControlType.py`

## Screen 与路径

| 接口 | 作用 | 返回 |
| --- | --- | --- |
| `gui.get_top_screen()` | 当前栈顶 screen 短名 | `str` |
| `gui.get_all_screen_fullnames()` | 当前已加载 screen 完整名 | `list[str]` |
| `gui.is_ui_screen_in_screen_stack(screenName)` | 判断 screen 是否在栈中 | `bool` |
| `gui.get_client_ui_screen_size()` | UI 逻辑尺寸 | `(w, h)` |
| `gui.get_client_screen_size()` | 客户端窗口像素尺寸 | `(w, h)` |

控件定位依赖：

```text
screen_name + component_path
```

示例：

```python
screen = "hud.hud_screen"
path = "/variables_button_mappings_and_controls/safezone_screen_matrix/inner_matrix/safezone_screen_panel/root_screen_panel"
```

`component_path` 必须是真实控件路径。盲试 `/`、`.`、`/root` 通常拿不到子节点。

## 控件树读取

| 接口 | 作用 | 风险 |
| --- | --- | --- |
| `gui.get_children_name_from_parent(screen, path)` | 返回直接子节点名称，不递归 | 低 |
| `gui.get_all_children_path_from_parent(screen, path)` | 返回所有子节点路径，递归 | 高 |
| `gui.recursively_get_children(screen, path)` | 递归获取子节点 | 高 |
| `gui.get_control_def_type(screen, path)` | 获取控件类型枚举 | 低 |

实测：

```json
{
  "screen": "hud.hud_screen",
  "path": "/variables_button_mappings_and_controls/safezone_screen_matrix/inner_matrix/safezone_screen_panel/root_screen_panel",
  "children_len": 2,
  "children_sample": ["root_panel", "camera_renderer"]
}
```

`root_panel` 再展开一层有 31 个直接子节点，其中包含大量隐藏或 0 尺寸节点。因此给 AI 的默认视图应支持：

- `visible_only=true`
- `nonzero_size_only=true`
- `include_hidden_summary=true`，只返回隐藏节点数量和名称摘要

## 控件类型枚举

来自 `UIControlType.py`：

| 值 | 类型 |
| --- | --- |
| `0` | `Button` |
| `1` | `Custom` |
| `2` | `CollectionPanel` |
| `3` | `Dropdown` |
| `4` | `EditBox` |
| `5` | `Factory` |
| `6` | `Grid` |
| `7` | `Image` |
| `8` | `InputPanel` |
| `9` | `Label` |
| `10` | `Panel` |
| `11` | `Screen` |
| `14` | `ScrollView` |
| `15` | `SelectionWheel` |
| `16` | `Slider` |
| `18` | `StackPanel` |
| `19` | `Toggle` |
| `23` | `Combox` |
| `24` | `Layout` |
| `25` | `StackGrid` |
| `27` | `RichText` |
| `31` | `Unknown` |

实测中 `pause.pause_screen` 的若干候选根路径返回 `32`，不在当前 SDK 枚举里，应作为 `EngineUnknown32` 保留，不要直接丢弃。

## 几何结果字段

| 接口 | 作用 | 返回 |
| --- | --- | --- |
| `gui.get_size(screen, path)` | 运行时计算后的尺寸 | `(w, h)` |
| `gui.get_position(screen, path)` | 相对父控件坐标 | `(x, y)` |
| `gui.get_global_position(screen, path)` | UI 屏幕坐标 | `(x, y)` |
| `gui.get_visible(screen, path)` | 当前节点可见性 | `bool` |
| `gui.get_min_size(screen, path)` | 最小尺寸 | `(w, h)` |
| `gui.get_max_size(screen, path)` | 最大尺寸 | `(w, h)` |
| `gui.get_clips_children(screen, path)` | 是否裁剪子节点 | `bool` |

实测 `root_screen_panel`：

```json
{
  "type": "Panel",
  "visible": true,
  "size": [487.0, 272.0],
  "position": [0.0, 0.0],
  "global_position": [0.0, 0.0]
}
```

## 布局表达式字段

这是最适合给 AI 做 JSON UI 迭代的接口组。

| 接口 | 作用 | 返回 |
| --- | --- | --- |
| `gui.get_size_x(screen, path)` | 宽度表达式 | `dict` |
| `gui.get_size_y(screen, path)` | 高度表达式 | `dict` |
| `gui.get_position_x(screen, path)` | X 坐标表达式 | `dict | None` |
| `gui.get_position_y(screen, path)` | Y 坐标表达式 | `dict | None` |
| `gui.get_anchor_from(screen, path)` | 相对父节点的锚点 | `str` |
| `gui.get_anchor_to(screen, path)` | 自身锚点 | `str` |

`get_size_x/y` 返回结构：

```json
{
  "absoluteValue": 0.0,
  "relativeValue": 1.0,
  "followType": "parent",
  "fit": false
}
```

含义：

```text
实际值 = absoluteValue + relativeValue * followValue
```

`followType` 常见值：

| 值 | 含义 |
| --- | --- |
| `none` | 不跟随，只使用绝对值 |
| `parent` | 跟随父控件宽/高 |
| `children` | 跟随所有子节点尺寸和 |
| `maxChildren` | 跟随最大子节点尺寸 |
| `maxSibling` | 跟随最大兄弟节点尺寸 |
| `x` | 跟随自身宽度，仅部分轴有效 |
| `y` | 跟随自身高度，仅部分轴有效 |

`fit=true` 表示自适应父控件，通常等价于填满父节点。

锚点值：

```text
top_left, top_middle, top_right,
left_middle, center, right_middle,
bottom_left, bottom_middle, bottom_right
```

## 文本控件字段

Label / RichText 可读取：

| 接口 | 作用 |
| --- | --- |
| `gui.get_text(screen, path)` | 文本内容 |
| `gui.get_text_color(screen, path)` | 文本颜色 |
| `gui.get_text_alignment(screen, path)` | 水平对齐：`left/right/center` |
| `gui.get_text_shadow(screen, path)` | 是否有阴影 |
| `gui.get_text_line_padding(screen, path)` | 行距 |

## 图片控件字段

Image 可设置和部分读取：

| 接口 | 作用 |
| --- | --- |
| `gui.set_sprite(screen, path, texture_path)` | 设置贴图 |
| `gui.set_sprite_color(screen, path, color)` | 设置图片颜色 |
| `gui.set_sprite_gray(screen, path, gray)` | 灰度 |
| `gui.set_sprite_uv(screen, path, uv)` | UV 起点 |
| `gui.set_sprite_uv_size(screen, path, uv_size)` | UV 尺寸 |
| `gui.set_sprite_clip_ratio(screen, path, clip_ratio)` | 裁剪比例 |
| `gui.get_clip_direction(screen, path)` | 裁剪方向 |
| `gui.get_clip_offset(screen, path)` | 裁剪偏移 |

目前源码里没有看到通用 `get_sprite_texture`，所以运行时未必能反查贴图路径。后续最好结合源 JSON 静态解析补齐 `texture/uv/uv_size/nine_slice`，运行时 API 负责验证尺寸、坐标和可见性。

## 列表与滚动容器

| 接口 | 作用 |
| --- | --- |
| `gui.get_grid_dimension(screen, path)` | Grid 维度 |
| `gui.get_grid_item_name(screen, path, x, y)` | Grid 单元名称 |
| `gui.get_stack_panel_orientation(screen, path)` | StackPanel 方向 |
| `gui.get_scroll_view_pos(screen, path)` | ScrollView 当前滚动位置 |
| `gui.get_scroll_view_percent_value(screen, path)` | ScrollView 滚动百分比 |

`ScrollViewUIControl` 里还约定了内容路径：

- touch: `/scroll_touch/scroll_view/panel/background_and_viewport/scrolling_view_port/scrolling_content`
- mouse: `/scroll_mouse/scroll_view/stack_panel/background_and_viewport/scrolling_view_port/scrolling_content`

