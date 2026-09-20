# 06 · 插件声明、清单与加载

上级索引：[README.md](README.md)

## 1. 信任模型（规范）

**规范：宿主禁止自动扫描任何目录来发现插件。插件必须由用户在 `.mcdev.json` 中逐条显式声明，未声明的动态库一律不加载。**

理由：

- 插件在进程内运行、拥有与 mcdk 相同的权限，"把 dll 丢进某个目录就会被执行"是不可接受的默认行为；
- 显式声明使**信任决策由用户做出且可审计**，`.mcdev.json` 随项目进入版本控制，改动在 code review 中可见；
- 避免同名插件在多个搜索路径间覆盖时产生的"到底加载了哪一个"问题；
- 与 [01-overview.md](01-overview.md) §3 的非目标一致：既然 v1 不做沙箱与权限强制，就必须把信任边界前移到声明这一步。

由此，插件清单中的 `permissions` 字段（见 §3）**仅用于在用户添加插件时向其展示**，运行期不做拦截。

## 2. `.mcdev.json` 中的声明

在现有 `.mcdev.json` 顶层新增 `plugins` 数组，顺序即声明顺序：

```jsonc
{
    // ... 现有字段 ...
    "plugins": [
        {
            "enable": true,
            "path": "./plugins/my-plugin",
            "id": "com.example.my-plugin",
            "config": {
                "watch_extra_dirs": ["./tools"]
            }
        },
        {
            "enable": false,
            "path": "D:/dev/wip-plugin/build/wip_plugin.dll"
        },
        {
            "enable": true,
            "path": "~/.mcdk/plugins/team-workflow"
        }
    ]
}
```

### 2.1 字段定义

| 字段 | 类型 | 必填 | 说明 |
| --- | --- | --- | --- |
| `enable` | bool | 是 | `false` 时宿主**禁止**加载该动态库，仅在 `mcdk plugin list` 中列出 |
| `path` | string | 是 | 插件目录（内含 `plugin.json`）或直接指向动态库文件。见 §2.2 |
| `id` | string | 否 | 期望的插件 id。填写后，与清单中 `id` 不符则拒绝加载并报错 |
| `config` | 任意 JSON | 否 | 传给该插件的配置，原样序列化后透传，插件通过 `mcdk.core` 的 `get_config` 取回文本自行解析。**这是「同一个插件二进制按不同参数声明多次」的基础。** 未写时插件收到字面量 `"null"` |
| `priority` | int | 否 | 覆盖默认的声明顺序，小者先加载 |

`enable` 与 `path` 之外全部可选，最小形态就是 `{ "enable": true, "path": "./plugins/foo" }`。

### 2.2 路径解析（规范）

1. 绝对路径按原样使用；
2. 以 `~/` 开头的路径展开为当前用户主目录；
3. 其余相对路径一律相对于 **`.mcdev.json` 所在目录**，而非进程工作目录；
4. 若 `path` 指向目录，宿主在其中查找 `plugin.json`；找不到则报错，**禁止**回退为在该目录中猜测动态库文件名；
5. 若 `path` 直接指向动态库文件，则跳过清单，使用内嵌于 `mcdk_plugin_desc` 的元数据（此形态下无法声明依赖与多平台产物，仅建议用于本地开发调试）。

### 2.3 `id` 的用途

`id` 是**防替换**手段，不是安全边界：填写后宿主会与清单中的 `id` 比对，防止 `path` 指向的内容被换成另一个插件（例如目录被重用、符号链接被改指）。不填则不校验。

## 3. `plugin.json` 清单

**规范：宿主必须在加载动态库之前完成清单解析与 ABI 校验。** 这样才能在不 `LoadLibrary` 的前提下列出插件、显示禁用状态、按依赖排序、向用户展示 `permissions`。Godot 的 `.gdextension` 采取同一策略，并按平台 + 构建配置分列库路径——对本项目的 MSVC debug/release 运行库分歧尤其有用。

### 3.1 目录布局

```text
<plugin-dir>/
  plugin.json
  bin/windows-x86_64/<name>.dll
  assets/
```

### 3.2 格式

```jsonc
{
    "schema": 1,
    "id": "com.example.my-plugin",
    "name": "My Plugin",
    "version": "1.0.0",
    "abi": { "major": 1, "minor": 0 },
    "entry_symbol": "mcdk_plugin_entry",
    "min_stage": "register",
    "libraries": {
        "windows.x86_64": "bin/windows-x86_64/my_plugin.dll",
        "windows.x86_64.debug": "bin/windows-x86_64/my_plugin_d.dll"
    },
    "dependencies": [
        { "id": "com.example.base", "version": ">=0.3.0" }
    ],
    "permissions": ["game.execute_code", "fs.project_write"],
    "config_schema": { "$schema": "...", "type": "object", "properties": { } },
    "description": "..."
}
```

`config_schema` 描述 `.mcdev.json` 中该插件 `config` 字段的结构，供 IDE 补全与宿主校验使用。

### 3.3 库选择

按 `<platform>.<arch>[.<config>]` 由具体到通用回退：宿主先找 `windows.x86_64.debug`，未命中则退到 `windows.x86_64`。无匹配项则该插件加载失败并报告缺少当前平台产物。

## 4. 加载流程（规范）

```text
1. 解析 .mcdev.json 的 plugins 数组，保留 enable == true 的条目
2. 逐条解析 plugin.json（或读取直指的动态库路径）
3. 校验 id（若声明）
4. 校验 abi.major 是否等于宿主；abi.minor 是否 <= 宿主
5. 在「已声明且已启用」的集合内做依赖拓扑排序
6. 按排序结果依次 LoadLibrary → 解析 entry_symbol → 调用入口 → 校验 plugin_desc
7. 进入生命周期阶段推进
```

失败处理（规范）：

| 情形 | 行为 |
| --- | --- |
| `path` 不存在 | 报错并跳过该插件，**不**终止 mcdk 启动 |
| ABI major 不符 | 报错并跳过，错误信息必须同时打印插件要求与宿主提供的版本号 |
| ABI minor 高于宿主 | 报错并跳过，提示用户升级 mcdk |
| `id` 不符 | 报错并跳过，提示内容可能已被替换 |
| 依赖不在已启用集合内 | 报错并跳过，**禁止**自动去别处寻找该依赖，提示用户手动加入 `.mcdev.json` |
| 依赖成环 | 整个环中的插件全部跳过 |
| 入口返回失败或抛异常 | 回滚该插件已注册的全部条目，跳过 |

任何单个插件的失败都**禁止**影响其余插件与 mcdk 主流程。

## 5. CLI

```bash
mcdk plugin list                 # 列出 .mcdev.json 中的声明及其解析结果
mcdk plugin enable <id|path>     # 将对应条目的 enable 置 true
mcdk plugin disable <id|path>    # 置 false
mcdk plugin add <path>           # 追加一条声明；先解析清单并展示 permissions 供用户确认
mcdk plugin remove <id>          # 移除声明
```

`add` **必须**在写入前把该插件声明的 `permissions` 与 `id`、`version` 打印给用户确认——这是 §1 信任模型中用户做决策的那一刻。

以上命令会改写 `.mcdev.json`，需与现有配置写回逻辑（`tryUpdateUserGamePath`）共用同一套保留格式的读改写实现，详见 [10-roadmap.md](10-roadmap.md) §4 待定问题 1。

## 6. 与用户级全局插件的关系

v1 **不提供**用户级全局插件列表（如 `~/.mcdk/config.json`）。所有声明都在项目的 `.mcdev.json` 中。

这意味着"我希望我的插件在所有项目里都生效"这一需求在 v1 需要用户在每个项目中各写一遍。是否引入全局列表见 [10-roadmap.md](10-roadmap.md) §4 待定问题 6——引入时必须保持"显式声明"这一性质，不得退化为目录扫描。
