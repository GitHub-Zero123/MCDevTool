# Shader 重载

MCDK 提供了开发期 Shader 自动热更新能力，用于快速验证资源包中的着色器修改。

启用 `auto_hot_reload_shaders` 后，MCDK 会监听资源包 `shaders` 目录下的文件变化，并在回到游戏前台时调用底层单文件 Shader 重载接口。

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

## 自动热更新参数

自动热更新会把发生变化的文件转换为相对于资源包 `shaders` 目录的路径，不带前导 `/`。

```text
entity.fragment
effects/bloom.fragment
```

如果 Shader 文件实际位于：

```text
R/shaders/effects/bloom.fragment
```

那么传给底层接口的参数会是：

```text
effects/bloom.fragment
```

## 注意

单文件 Shader 重载仍然可能耗时较长，因为引擎可能会编译并校验相关 Shader 状态。

如果修改内容涉及共享 include、材质、渲染管线配置，或多个 Shader 文件之间存在依赖关系，底层引擎仍可能触发相关 Shader 编译或报出依赖侧错误。这类情况通常需要结合游戏日志判断，而不是只看被修改的单个文件。
