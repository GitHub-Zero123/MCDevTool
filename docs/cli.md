# MCDK CLI

## 定位

CLI 只服务一件事：让 Agent 和脚本可控地驱动游戏。项目脚手架、环境查看、插件管理都不属于它的范围，此前的 `create`、`envinfo`、`plugin` 命令已经移除，插件声明直接手写进 `.mcdev.json`。

两种入口互不影响：

| 用法 | 面向 | 行为 |
| --- | --- | --- |
| `mcdk` | 编辑器任务、手动使用 | 交互式启动：缺配置时创建，路径失效时询问并写回 |
| `mcdk <命令>` | Agent、脚本、CI | 不询问、不改配置文件，用退出码报告结果 |

不带参数的 `mcdk` 与之前完全一致：链接全局包目录、不写会话文件、不绑定游戏进程，游戏退出后返回 0。CLI 始终编译，没有开关。

## 典型流程

```bash
mcdk run --session a --detach
mcdk wait --session a
mcdk call execute_code --session a --args '{"code":"1+1"}'
mcdk stop --session a
```

多开时用不同的会话名即可，每个会话有自己的包、存档和 MCP 端口：

```bash
mcdk run --session a --detach
mcdk run --session b --detach --config tests/另一份配置.json
```

## 查看用法

```bash
mcdk --help
mcdk help run
mcdk help execute_code
```

工具说明由工具定义直接生成，内置工具和项目插件在 `plugin.json` 中声明的工具都会列出。新增或修改工具后，帮助内容自动更新，不需要另外维护。

## `mcdk run`

```bash
mcdk run [--project DIR] [--config FILE] [--set KEY=VALUE]... [--session NAME] [--lane LEVEL] [--lane-root DIR] [--detach]
```

| 参数 | 说明 |
| --- | --- |
| `--project` | 项目目录。配置里的相对路径、插件路径、`.mcdev/` 都以它为基准，默认当前目录 |
| `--config` | 要读取的配置文件，默认 `<project>/.mcdev.json`。相对路径相对调用者所在目录 |
| `--set` | 覆写配置项，可重复，按出现顺序生效 |
| `--session` | 隔离会话名，只能包含字母、数字、`_` 和 `-`。不指定时会话名为 `default`，且不隔离 |
| `--lane` | Lane 级别：`addons`、`worlds`（默认）、`full`，需要 `--session` |
| `--lane-root` | Lane 目录，默认 `<project>/.mcdev/lanes/<会话名>` |
| `--detach` | 后台运行：游戏进程创建后打印会话信息并返回，输出写入 `.mcdev/sessions/<名称>.log` |

`--config` 只决定读哪个文件，不改变相对路径的基准。这样放在别处的测试配置里，`"./"` 仍然指项目目录。配置文件不存在时直接失败，不会创建。

路径支持中文。配置文件只由 mcdk 读取，不会传给游戏。

### `--set`

键用 `.` 进入嵌套对象，缺失的层级会自动创建。值能解析成 JSON 就按 JSON，否则按字符串：

```bash
mcdk run --set reset_world=true
mcdk run --set world_name=冒烟测试
mcdk run --set mcp_server_config.server_port=19140
mcdk run --set included_mod_dirs='["./","D:/共用模组"]'
```

值是数字形式但需要字符串时，写成 JSON 字符串：`--set 'world_name="123"'`。

### 隔离会话

`--session NAME` 在配置之上做这几件事：

- 在 [Lane](lane.md) 里运行：游戏的数据根目录指向这个会话自己的目录，按 `--lane` 级别决定独享包目录、存档还是整个数据目录。
- 包只链接到 Lane 的包目录，存档里不放任何联接，配置里的 `pack_link_mode` 不生效。
- `addons` 级别的存档与真实目录共享，存档目录变成 `<world_folder_name>__NAME`；`worlds`、`full` 级别沿用原名。
- 启用 MCP，并分配一个空闲端口。用 `--set mcp_server_config.server_port=...` 可以指定端口。
- `mc_input` 默认使用后台投递。

隔离会话之间互不影响，同 UUID 的包也能并存，比如同一项目的两个 worktree。无参 `mcdk` 和不带 `--session` 的 `run` 仍使用真实的全局包目录，影响不到隔离会话。机制、目录布局和删除方式见 [lane.md](lane.md)。

### 与无参启动的差异

- **游戏路径：** 配置中的路径无效时使用已发现的最新版本，只影响本次，不写回。
- **MCP：** 已启用但端口监听失败时中止启动，游戏不会运行。无参启动只打印红色警告并继续，因为联机调试的子进程实例可能与主实例共用配置。
- **进程绑定：** mcdk 退出时游戏随之结束。无参启动不绑定，关掉 mcdk 不影响游戏。
- **输出：** stdout 不缓冲，管道另一端能实时读到日志；不打印启动 Logo。

## `pack_link_mode`

配置项，决定 Addon 链接到哪里：

| 取值 | 位置 | 说明 |
| --- | --- | --- |
| `global`（默认） | `games/com.netease/behavior_packs`、`resource_packs` | 与之前的行为一致。每次启动先清空这两个目录，同时只适合运行一个普通实例 |
| `world` | 存档内的 `behavior_packs`、`resource_packs` | 不碰全局目录。链接保留在存档里，记录在 `.mcdk_pack_links.json`，下次启动时重建 |

联机调试的子进程模式不做任何链接，依赖主实例的全局包，所以主实例要保持 `global`。隔离会话不使用这个配置项，见上文。

**警告：不要在游戏界面里删除 `world` 模式的存档。** 这类存档里始终有指向源码的目录联接，实测游戏删除存档时会顺着联接把源码一起删掉。需要重建存档时用 `reset_world`，mcdk 只会摘掉链接。需要隔离时请改用 Lane，它不在存档里放联接。

## 会话命令

这些命令的 stdout 都是 JSON。不带 `--session` 时，运行中的会话恰好只有一个才能省略。

| 命令 | 说明 |
| --- | --- |
| `mcdk ps` | 列出本项目的会话 |
| `mcdk wait [--state ready\|exited] [--timeout 秒]` | 等待进入存档或退出。默认等 `ready`，超时 120 秒。会话还没启动时会继续等 |
| `mcdk call <工具> [--args JSON] [--args-file FILE] [--output FILE] [--timeout 秒]` | 调用工具。截图等大结果用 `--output` 写入文件 |
| `mcdk stop [--all] [--timeout 秒]` | 结束游戏并等 mcdk 退出。返回后端口已释放，可以马上重开同名会话 |
| `mcdk lane remove --session NAME [--lane-root DIR]` | 删除会话的 Lane 目录，并摘掉空闲的入口槽位。遇到联接只摘链接，不会删到真实游戏数据。会话仍在运行时拒绝 |

`stop` 直接结束游戏进程，不走正常保存流程。原因见下文的“退出码”。

会话状态：

| 状态 | 含义 |
| --- | --- |
| `process_started` | 游戏进程已创建，还没进入存档 |
| `game_ready` | 已进入存档，工具可用 |
| `game_unavailable` | 不在存档内，或没有包含调试 MOD |
| `exited` | 已退出，`exitCode` 为游戏退出码，`stopped` 表示由 `mcdk stop` 结束 |
| `lost` | mcdk 进程已不在，但记录没有写完 |

## 退出码

| 退出码 | 含义 |
| --- | --- |
| 0 | 成功 |
| 2 | 命令行用法错误 |
| 3 | 配置错误，或找不到可用的会话 |
| 4 | 启动失败：MCP 端口被占用、进程创建失败、被插件否决 |
| 5 | 游戏不是由 `mcdk stop` 结束的，原始退出码打印到 stderr |
| 6 | 等待超时 |
| 7 | 工具调用返回错误 |

退出码 5 不一定代表崩溃。实测关闭游戏窗口时，游戏本身就会以 `0xC0000005` 或 `0xC0000409` 退出，所以外部无法区分“窗口被关闭”和“真的崩溃”。自动化流程应当用 `mcdk stop` 结束游戏，这样 `run` 返回 0；只要看到 5，就说明游戏在预期之外退出了。

## 后台输入

`mc_input` 有两种投递方式，用 `delivery` 选择：

| 取值 | 行为 | 默认用于 |
| --- | --- | --- |
| `sendinput` | 系统输入。抢前台、移动真实鼠标，同一时间只能操作一个窗口 | 无参启动、不带 `--session` 的 `run` |
| `post` | 把消息投递到游戏窗口。不抢前台、不动鼠标，多个实例可以同时操作 | 隔离会话 |

```bash
mcdk call mc_input --session a --args '{"op":"/key","args":{"keys":"w","hold_ms":1500}}'
mcdk call mc_input --session a --args '{"op":"/click","args":{"at":[0.24,0.56],"delivery":"post"}}'
```

`post` 模式的限制：

- **视角：** 游戏用原始鼠标输入转视角，消息投递不进去，`look` 会直接报错。改用 `execute_code` 调 `CreateRot(playerId).SetRot((pitch, yaw))`。
- **窗口状态：** 窗口不能最小化，否则按键无效，截图也是旧画面。
- **触屏布局：** 窗口失焦时投递鼠标事件，游戏可能切到触屏布局，界面点击不受影响，但看到的 UI 和键鼠模式不同。
- **拖拽：** `drag` 在失焦时不可靠，实测物品没有按预期移动。搬运物品请用两次 `click`：先点物品，再点目标格。
- **修饰键：** `focus`、`ime`、`restore_cursor` 在这个模式下不生效。

## 与 MCP 的关系

CLI 不替代 MCP，两者共用同一份工具定义和同一个服务。MCP 面向已连接的 Agent；CLI 还负责 MCP 做不到的部分：启动、等待、结束，以及在多个实例之间选择目标。

## 已知限制

- `mc_input` 的后台投递限制见下文“后台输入”。
- `addons`、`worlds` 级别的 Lane 仍与真实目录共享游戏设置和日志；需要完全独立请用 `full`。
- 不带 `--session` 的 `run` 不要同时运行多个：全局包清理发生在 MCP 端口检查之前。
- Lane 不会自动删除，用 `mcdk lane remove` 清理。
- Lane 依赖 `mcdev-lane-shim.dll`，只有 CMake 会构建它；缺少时隔离会话拒绝启动。
