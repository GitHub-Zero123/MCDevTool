# Shader 重载

MCDK 提供了开发期 Shader 重载能力，用于快速验证资源包中的着色器修改。

只修改单个 Shader 源文件时，优先使用单文件重载；如果修改了共享 include、材质、渲染管线配置，或一次修改了多个互相依赖的 Shader 文件，应使用全量 Shader 重载。

## API 参数规则

底层 ModSDK API 是：

```python
import clientlevel

clientlevel.reload_one_shader(shaderName, True)
```

`shaderName` 用于定位当前已加载资源包 `shaders` 目录下的 Shader 文件。

根据运行时实测，参数规则如下：

- 使用相对于 `shaders` 目录的路径。
- 嵌套目录使用 `/` 作为分隔符。
- 不要包含 `shaders/` 前缀。
- 底层 `clientlevel.reload_one_shader` 可以接受带前导 `/` 或不带前导 `/` 的路径。
- 面向工具和用户命令时，统一推荐不带前导 `/`，便于和其他参数保持一致。
- 定位具体文件时建议包含扩展名。虽然部分引擎路径不带扩展名也可能解析成功，但这不适合作为稳定约定。

有效示例：

```python
clientlevel.reload_one_shader("entity.fragment", True)
clientlevel.reload_one_shader("/entity.fragment", True)
clientlevel.reload_one_shader("effects/bloom.fragment", True)
clientlevel.reload_one_shader("/effects/bloom.fragment", True)
```

错误示例：

```python
clientlevel.reload_one_shader("shaders/entity.fragment", True)
clientlevel.reload_one_shader("/shaders/effects/bloom.fragment", True)
```

## MCP 工具参数

MCP 工具 `reload_single_shader` 使用面向用户的 `file_name` 参数。该参数应传入相对于资源包 `shaders` 目录的路径，不带前导 `/`。

```text
reload_single_shader(file_name="entity.fragment")
reload_single_shader(file_name="effects/bloom.fragment")
```

如果 Shader 文件实际位于：

```text
R/shaders/effects/bloom.fragment
```

那么 MCP 参数应传：

```text
effects/bloom.fragment
```

## 注意

单文件 Shader 重载仍然可能耗时较长，因为引擎可能会编译并校验相关 Shader 状态。

如果修改内容涉及共享 include、材质、渲染管线配置，或多个 Shader 文件之间存在依赖关系，`reload_all_shaders` 通常更可靠。
