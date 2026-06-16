# 游戏内 UI 探针安全策略

这份文档记录调用游戏 UI API 时必须遵守的安全边界。我们已经遇到过一次批量查询 HUD 大节点导致游戏崩溃的情况，所以后续工具必须默认保守。

## 安全调用顺序

刚启动游戏后，不要直接递归扫描大节点。推荐顺序：

1. `gui.get_all_screen_fullnames()`：确认 screen 是否加载。
2. `gui.get_control_def_type(screen, path)`：确认路径存在和控件类型。
3. `gui.get_children_name_from_parent(screen, path)`：只拿直接子节点名。
4. 对少量已确认路径调用 `get_size/get_position/get_global_position/get_visible`。
5. 对少量已确认路径调用 `get_size_x/y/get_position_x/y/get_anchor_from/to` 获取布局表达式。

## 低风险接口

这些接口可以用于常规探针，但仍应限制批量数量：

- `gui.get_top_screen()`
- `gui.get_all_screen_fullnames()`
- `gui.get_client_ui_screen_size()`
- `gui.get_client_screen_size()`
- `gui.get_control_def_type(screen, path)`
- `gui.get_children_name_from_parent(screen, path)`
- `gui.get_size(screen, path)`
- `gui.get_position(screen, path)`
- `gui.get_global_position(screen, path)`
- `gui.get_visible(screen, path)`
- `gui.get_size_x/y(screen, path)`
- `gui.get_position_x/y(screen, path)`
- `gui.get_anchor_from/to(screen, path)`

## 高风险接口

这些接口不要裸调大根节点：

- `gui.get_all_children_path_from_parent(screen, path)`
- `gui.recursively_get_children(screen, path)`
- `gui.get_property_bag_value(screen, path)`

如果必须使用，应由 MCP Tool 内部加保护：

| 参数 | 建议默认值 | 说明 |
| --- | --- | --- |
| `max_depth` | `2` 或 `3` | 默认只看浅层结构 |
| `max_nodes` | `80` 到 `300` | 超过即截断 |
| `timeout_ms` | 较短 | 防止游戏侧长时间卡住 |
| `fields` | `summary` | 默认只返回轻量字段 |
| `visible_only` | `true` | 减少隐藏模板干扰 |

## 禁止模式

不要在一次 `execute_code` 里对多个大路径同时查询以下字段：

```python
gui.get_all_children_path_from_parent(...)
gui.recursively_get_children(...)
gui.get_property_bag_value(...)
```

不要从 `/`、`.`、`/root` 盲目递归。JSON UI 的有效路径必须先通过业务代码、静态 JSON 或已知父节点逐层确认。

## 推荐分片策略

树读取应采用逐层展开：

```text
get_children_name(parent)
-> 对每个 child 取 type/visible/size
-> 根据 max_depth 和 max_nodes 决定是否继续展开
```

这样即使 HUD 根节点下有大量隐藏节点，也不会一次把上下文或游戏进程压垮。

## 输出要求

所有树类 tool 都必须返回：

- `truncated`
- `truncated_reason`
- `visited_nodes`
- `returned_nodes`
- `max_depth`
- `max_nodes`

完整 dump 应写到文件或 artifact，只在响应中返回路径和摘要。

