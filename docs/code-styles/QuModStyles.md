# Python 代码规范
目标是**高可维护性**：读代码的人不需要上下文就能看懂一个文件在干什么，在长期工作中能够高效维护 / 便于接手。

## 1. 运行环境

- 网易 ModSDK 脚本运行时是 **Python 2.7**：不用 f-string、类型注解、`typing`、`nonlocal`、`yield from`；字符串格式化用 `"%s" % x`。
- 类型只写 `# type:` 注释，如 `self.warnings = []  # type: list[StrikeWarning]`。
- 纯逻辑代码（`Core.py`）必须同时能在 Python 3 与 2.7 下运行，便于脱离游戏单测。
- 只用最基础的标准库：计时 `time.time()`，随机 `random` 的普通函数或 `Random(seed)`；不用 `SystemRandom`、`os.urandom`、`uuid`、`secrets`、`perf_counter`、`time.clock`、`time.monotonic`。
- 大多数时候返回给引擎的文本用 UTF-8 `str`，不要 `u""`。
- 每个 `.py` 文件第一行 `# -*- coding: utf-8 -*-`。

## 2. 命名

| 对象 | 风格 | 例子 |
| --- | --- | --- |
| 文件夹（含 Feature 目录） | 大驼峰 | `Features/Disasters/Flood/` |
| 文件 | 大驼峰 | `Server.py`、`Core.py`、`WaterLevel.py` |
| 类 / 接口 / 枚举类 | 大驼峰（接口 `I` 前缀） | `LayerFader`、`IOnHurt` |
| 函数 / 方法 / 变量 / 参数 | 小驼峰 | `warnProgress`、`playerId` |
| 内部成员 / 模块内部函数 | 单下划线 `_` 开头 + 小驼峰 | `_lastNow`、`_onStrikeCommand` |
| 全私有成员（很少用） | 双下划线 `__` 开头 + 小驼峰 | `__cache` |
| 常量 / 枚举值 | 全大写下划线（模块私有加 `_`） | `FLOOD_RAIN_LEVEL`、`_PROBE_INTERVAL` |
| 资源标识 | 小写下划线 + 命名空间 `kidtest` | `kidtest:sandbag`、`query.mod.kidtest_state` |

- 下划线约定：
  - 方法、函数、变量一律小驼峰；常量一律全大写下划线。
  - `_` 开头表示“内部属性，但允许外部访问”：不属于对外承诺的 API，但同包内的协作者、测试、调试命令需要时可以直接读写，不必为此额外写 getter / setter。
  - `__` 开头表示全私有（触发 Python 的名称改写），**很少使用**：Python 的私有本来就是伪私有，改写后外部和子类访问、测试、调试都不方便，远不如 `_` 好用。只有确实要避免子类同名冲突时才用。
- 命令名带前缀 `ka_`（如 `/ka_season`），命令文件 `mods/BP/netease_commands/kidtest/<名称>.json`，文件名等于命令名。
- 枚举与字符串常量的定义规则见第 4 节。
- 存档键写显式字面常量，不依赖模块路径，重构搬文件不会丢档。

## 3. 排版：不挤压

可读性优先于行数。

```python
# 不要
if playerId is None: return False, "只能由玩家执行"
for x in items: total += x.count
class A(object): pass
value = a if a else (b if b else c)   # 多层嵌套三元

# 要
if playerId is None:
    return False, "只能由玩家执行"

for item in items:
    total += item.count
```

- 禁止 `if xxx: ...`、`for ...: ...`、`while ...: ...`、`def f(): return x` 压成一行。分号拼多条语句也禁止。
- 三元表达式只用于一层、一眼看懂的赋值，嵌套就改写成 `if`。
- 长度 120 列以内，超长的参数列表、字典、条件按语义换行，一行一项，尾部不要逗号堆叠到难读。
- 字符串统一用双引号 `"..."`，docstring 用 `"""..."""`。在 C / C++ / Java / Rust / Go 等语言里单引号表示单个字符（char），不是字符串；统一双引号可以避免跨技术栈阅读时产生歧义。只有字符串本身包含双引号、换成单引号能省掉转义时，才用单引号。
- 顶层函数 / 类之间空两行，类内方法之间空一行；import 分组：标准库、框架（QuModLibs）、同项目，组间空一行。
- 提前返回（guard clause）代替深层嵌套，函数嵌套超过三层就拆函数。
- 一个函数只做一件事，长度超过约 60 行就考虑拆分。

## 4. 分发与字面量

### 4.1 长 if 链改注册表

按类型、命令、状态分发的 `if / elif` 链，**分支超过约 4 个，或可预见会随新内容继续增长**（新灾害、新命令、新物品），就主动改成注册表：key → 处理函数（或处理类）的映射。新增一项只加一处注册，不改分发代码。

```python
# 不要：每加一种灾害都要回来改这里
def start(kind, args):
    if kind == DisasterKind.FLOOD:
        ...
    elif kind == DisasterKind.QUAKE:
        ...
    elif kind == DisasterKind.STORM:
        ...

# 要
_STARTERS = {}  # type: dict[int, function]


def starter(kind):
    def deco(fn):
        _STARTERS[kind] = fn
        return fn
    return deco


@starter(DisasterKind.FLOOD)
def _startFlood(args):
    ...


def start(kind, args):
    fn = _STARTERS.get(kind)
    if fn is None:
        print("[Disaster] 未注册的类型: %s" % kind)
        return False
    return fn(args)
```

- 分发处只做“查表 + 缺省处理”，查不到要打日志或明确返回失败，不静默跳过。
- 各分支只是数值不同、逻辑相同时，注册的是数据（放 `Profile.py` 的表），不是函数。
- 装饰器注册依赖模块被导入：注册所在的模块要在端侧入口显式 import，否则表里是空的。
- 两三个稳定、不会再长的分支直接写 `if`，不为它过度设计。

### 4.2 不散写字面量字符串

字面量字符串东一个西一个，改名要全局搜、拼错不报错，是维护灾难。

- 同一个字符串值用到两处及以上（事件名、RPC 路径、存档键、identifier、tag、UI 控件路径），必须集中定义为常量后引用。常量拼错是 `NameError`，裸字符串拼错只会静默不生效。
- 定义位置：双端共享的放 `Common.py`；Feature 内部的放模块顶部常量；可调的值放 `Profile.py`。
- **纯枚举 / 状态比较优先用 int**：值只用于判等、分支、做 key，不参与字符串拼接、显示或传给引擎时，用整数常量类，不用字符串。
- 只有值本身就要当字符串用时（引擎 API 参数、存档键、展示文本）才用字符串，并且同样集中定义。
- int 枚举需要可读日志时，配一张 `值 → 名字` 的映射表用于输出，而不是因此把枚举改成字符串。
- 一次性的日志文案、提示语可以直接写字面量。

```python
# 不要
self.state = "warning"
...
if self.state == "warnning":  # 拼错了，永远不成立，也不报错
    ...

# 要
class StrikeState(object):
    IDLE = 0
    WARNING = 1
    ACTIVE = 2


_STATE_NAMES = {
    StrikeState.IDLE: "IDLE",
    StrikeState.WARNING: "WARNING",
    StrikeState.ACTIVE: "ACTIVE",
}

self.state = StrikeState.WARNING
if self.state == StrikeState.WARNING:
    ...
```

## 5. 异常处理：非必要不 try

ModSDK 的接口大多是 C 封装，几乎不会主动抛异常。

- **默认不写 `try / except`。** 不要用它掩盖逻辑错误，也不要写 `except: pass`。
- 参数合法性靠显式判断：`if entityId is None: return`、`if not comp: return`，先判断再用。
- 只在这些情况才允许 `try`：解析外部不可信数据（用户输入的数字、配置/存档里的 JSON、网络文本）、文件读写、第三方库调用。`try` 范围尽量小，`except` 写具体异常类型，并且打印日志，不吞掉。
- 出错应该让它在开发期暴露（日志里看到堆栈），而不是被静默吞掉再在别处表现成莫名其妙的行为。

## 6. Feature 设计

业务按 Feature 组织，目录结构与参考 mod 一致：

```
mods/BP/KIDTest/
├── modMain.py            # 入口
├── Server.py / Client.py # 顶层入口：只导入各 Feature 的端侧入口，注释掉某行即关闭该 Feature
├── QuModLibs/            # 框架，不改动
└── Features/
    ├── Framework/        # 通用框架：Commands、EntityKit、Fx、Log 等
    ├── <分组>/<Feature>/ # 如 Disasters/Flood、Seasons/Calendar、Interface/Hud
    └── ...
```

每个 Feature 目录按职责分文件，不要写成一个大文件：

| 文件 | 职责 | 约束 |
| --- | --- | --- |
| `Profile.py` | 纯数据数值表、可调参数 | 不含逻辑；每个常量旁写清单位（秒 / 格 / 0~1）和调参含义 |
| `Common.py` | 双端共享的常量与协议（RPC 路径、事件名） | 只放常量，不导入 ModSDK |
| `Core.py` | **纯逻辑**，不导入 ModSDK / QuModLibs，时间由外部传入 | 可单测，是业务规则的主体 |
| `Server.py` / `Client.py` | 端侧入口：ModSDK 适配与 QuMod 接线 | 薄；只做“调接口 + 调 Core” |
| `EventBridge.py` | 引擎事件的集中 `@Listen` | 业务逻辑不散落监听 |
| 其他按名词命名（`Aura.py`、`Pool.py`） | 该 Feature 内的子模块 | 一个文件一个主题 |

- Feature 分组按职责取名，新 Feature 先放进最接近的现有分组；一批 Feature 明显不属于任何分组时才新增（带 `__init__.py` 写一行说明）。
- 子组名不要与其中的 Feature 同名。
- 每个模块顶部写 docstring：这个模块负责什么、对外提供哪些入口、关键约定；对外 API 在 docstring 里列出调用示例。
- 跨 Feature 只依赖对方的 `Server` / `Client` / `Common` 公开入口，跨 Feature 不依赖对方的 `_` 成员（同 Feature 内部协作、测试和调试可以直接访问）。
- 引擎事件只在 `EventBridge.py`（或端侧入口）集中监听。持续动画用按需监听，播完即取消监听；不要无条件每个脚本刻重算，低频刷新用节流。
- 日志统一走 `Features/Framework/Log.py`：高频 / 周期日志用 `Log.verbose`（默认关闭），错误和调试命令回显才直接 `print`。
- 注释写“为什么”和约束，不复述代码；中文注释，参数单位写明。

## 7. 测试

- 测试放根目录 `tests/`，文件名 `test_*.py`，开头 `import scriptPath  # noqa: F401` 后再导入 `KIDTest.Features...`。
- 测试不得导入 ModSDK；业务规则写进 `Core.py` 就是为了能测。
- 提交前用 Python 3 与 Python 2.7 各跑一遍。

## 8. 验证口径

分开标注【逻辑】纯逻辑测试通过 /【静态】JSON 与文档核对 /【实机】游戏内验证 /【规划】未实现。没在游戏里测过不得写“已验证”。不确定的 ModSDK 接口先查文档，查不到就标【待验证】并给出退路。
