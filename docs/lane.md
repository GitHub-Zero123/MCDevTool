# Lane

Lane 给每个游戏实例一个自己的数据根目录，再按级别决定哪些数据独享、哪些与真实目录共享。`mcdk run --session` 的隔离会话都运行在 Lane 里。

它不是沙盒：没有安全边界，游戏仍然可以访问任意路径。Lane 只改变一件事，就是游戏认为自己的 AppData 在哪里。

## 级别

| 级别 | 独享 | 共享 | 适用场景 |
| --- | --- | --- | --- |
| `addons` | 行为包、资源包目录 | 存档、设置、日志、缓存 | 只想让多个工程的包互不干扰，存档沿用真实目录 |
| `worlds`（默认） | 包目录和存档 | 设置、日志、缓存 | 多开测试。各实例可以使用同名存档，互不冲突 |
| `full` | 整个数据目录 | 无 | 需要干净环境，或不希望测试改动你的游戏设置 |

```bash
mcdk run --session a --lane worlds
mcdk run --session b --lane full --lane-root D:/工程/.mcdev/lanes/b
mcdk lane remove --session a
```

同一个 Lane 的级别建好后不能改。要换级别，先 `mcdk lane remove`。

## 机制

### 数据根目录重定向

`mcdev-lane-shim.dll` 在游戏入口点之前注入，拦截游戏取 Roaming AppData 的系统函数，返回 Lane 的入口目录：

- `SHGetKnownFolderPath(FOLDERID_RoamingAppData)`：游戏原生层取数据目录走这个。
- `SHGetSpecialFolderPathW(CSIDL_APPDATA)`：游戏也会调用，同样要接住。
- `SHGetFolderPathW/A`、`SHGetSpecialFolderPathA`：一并覆盖。

其他目录（Local AppData、文档等）原样放行。游戏进程的 `APPDATA` 环境变量也指向同一个入口，Python 层和原生层看到的是同一个路径。

主程序被 WinLicense 加壳，导入表在运行时才解析，所以没法改它的导入表。系统函数位于系统 DLL 里，不在壳内：

1. mcdk 以挂起状态创建游戏进程。
2. 用 APC 在主线程跑到入口点之前加载 shim。
3. `windows.storage.dll` 载入的那一刻安装钩子。这时游戏还没调用这些函数，壳也还没复制它们的开头。

### 失败即拒绝启动

shim 无法保证重定向时，会直接结束游戏，并用专门的退出码告诉 mcdk 原因：

- `MCDEV_LANE_EXIT_SETUP_FAILED`：环境缺失，或 MinHook 初始化失败。
- `MCDEV_LANE_EXIT_HOOK_FAILED`：`windows.storage.dll` 载入后钩子装不上。

mcdk 恢复主线程后会等 shim 发出就绪信号，超时或游戏先退出都会中止启动。缺少 `mcdev-lane-shim.dll` 时也会拒绝启动。

不存在“以为隔离了其实没有”的情况，安全不依赖任何事后清理。

### 入口槽位

游戏的 Python 模组加载器把模组脚本路径按 `/` 切开后，直接丢掉前 10 段，剩下的拼成模块名。见 ModSDK 中 `mod/common/minecraftMod.py` 的 `LoadWindowsAddonPy`：

```python
tempPyFilePathList = filePathName.split('/')[10:]
```

它默认前 10 段一定是 `C:/Users/<用户>/AppData/Roaming/MinecraftPE_Netease/games/com.netease/behavior_packs/<uuid>`。所以游戏看到的 AppData 必须**正好 5 段**，与盘符、与真实 Roaming 都无关。工程里的 Lane 目录通常更深，模块名会算错，调试 MOD 和你的脚本都会加载失败。

所以游戏实际使用的是一个 5 段的入口联接：

```text
%USERPROFILE%\.mcdk-lanes\<槽位编号>  →  <工程>/.mcdev/lanes/<会话名>
```

- **槽位复用：** 每个槽位对应一个系统命名互斥量，mcdk 运行期间持有，进程结束时由内核释放，被强杀也一样。启动时占一个空闲槽位，把联接改指到本次的 Lane。
- **不会泄漏累积：** 每次占用槽位和 `mcdk lane remove` 时，会摘掉其他空闲槽位留下的联接。`.mcdk-lanes` 里的入口数量不超过同时运行的实例数，不随工程、会话名或 worktree 增长。
- **不会误删：** 槽位位置上如果是真实目录，一律跳过、绝不删除。
- **用户目录段数不符：** 用户目录不是 `C:\Users\<用户>` 这样的 3 段时，入口凑不出 5 段，mcdk 会报错并拒绝启动。

入口只是联接，数据本身仍在工程的 Lane 目录里。顺带的好处是游戏看到的路径变短，不容易超过 Windows 的 260 字符限制。

### 目录布局

```text
<Lane 目录>/
  lane.json                         级别
  MinecraftPE_Netease/
    games/com.netease/
      behavior_packs/               独享，mcdk 把 Addon 链接到这里
      resource_packs/               独享
      <其他条目>                    worlds/addons：联接到真实目录
    minecraftWorlds/                worlds、full：独享；addons：联接到真实目录
    minecraftpe/、logs/ 等          worlds/addons：联接到真实目录；full：独享
    deviceid.txt 等散落文件         worlds/addons：首次从真实目录复制一份
```

- **包只链接到 Lane 的包目录：** 存档里不放任何联接，所以在游戏里删除存档不会波及源码。隔离会话不受配置里 `pack_link_mode` 的影响。
- **不会覆盖已有数据：** 准备布局时只替换联接，Lane 里已经存在的真实目录一律保留。

## 放在哪里

默认是 `<工程>/.mcdev/lanes/<会话名>`，mcdk 会生成 `.mcdev/lanes/.gitignore`，不会碰 `.mcdev/.gitignore`。

建议把 Lane 放在对应的工程或 worktree 目录下，让测试数据跟着代码走，切换 worktree 时互不混用。不要放进系统临时目录。用 `--lane-root` 指定别的位置时，mcdk 会在该目录里生成 `.gitignore`。

## 删除

`addons` 和 `worlds` 级别的 Lane 里有指向真实游戏数据的联接：`addons` 甚至包括你真实的存档目录。

请用 `mcdk lane remove` 删除。它遇到联接只摘链接本身，不会进入。

不要手动删除 Lane 目录，也不要删除整个 `.mcdev`。有些工具会顺着联接删到真实数据，例如 Windows PowerShell 5.1 的 `Remove-Item -Recurse`。`full` 级别没有这类联接。

## 构建

shim 是独立组件 `components/lane/shim`，依赖 MinHook v1.3.4（FetchContent 锁定版本与 SHA256，下载缓存与其他三方库共用）。CMake 会把 `mcdev-lane-shim.dll` 和 `licenses/lane/LICENSE-MinHook.txt` 放到 `mcdk.exe` 旁边，并让 `mcdk` 依赖它。仅支持 Windows x64，xmake 不构建这个组件。

## 排查

设置环境变量 `MCDEV_LANE_TRACE` 为一个文件路径后启动，shim 会把每次被重定向的调用及调用方模块追加到该文件（UTF-16）。
