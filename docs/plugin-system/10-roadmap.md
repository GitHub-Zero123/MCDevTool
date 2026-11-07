# 10 · 实施路线与待定问题

上级索引：[README.md](README.md)

## 1. 里程碑

| 阶段 | 内容 | 产出判据 |
| --- | --- | --- |
| M0 | 前置重构 A（`mcdk::runtime::Session`） | `launchGameExe` 拆分完成，现有测试全绿 |
| M1 | 前置重构 B（`McpToolRegistry`） | 内置 MCP 工具全部走注册表 |
| M2 | 冻结 `abi/core.h` + `abi/entry.h`，打通 loader | 只带 `mcdk.console`，端到端：读 `.mcdev.json` 声明 → 校验 → 入口 → `on_stage` → 卸载 |
| M3 | 异常屏障双向完成 + [09](09-compatibility.md) §1/§2 CI 矩阵 | **完成。** 一致性套件本地通过；`plugin_load_all_test` 从 `MCDEV_TEST_PLUGIN_PATHS` 读一组路径，CI 把五种工具链的产物一起载进同一个进程跑完整生命周期 |
| M4 | 事件总线 + v1 全部 9 条事件 | **完成。** 总线、9 个发射点、`MCDK_ENABLE_PLUGINS` 开关、A/B/C 三组基准（[12](12-performance.md) §6）均已落地，实测 B 组 约 2.3 ns/发射。详见 §1.1 |
| M5 | 补齐 v1 六张接口表，每张配一个 example | **完成。** 六张表全部可用，阶段窗口由 shim 强制执行。例子集中在 `examples/01-hello`，尚未按表拆成六个独立 example |
| M6 | `plugin.json` 完整字段、依赖拓扑排序、`mcdk plugin` 系列命令 | **完成。** 清单解析（含按平台选库、`entry_symbol`、`permissions`）、Kahn 拓扑排序（依赖缺失 / 版本不符 / 成环各自跳过）、五条 CLI 命令均已可用 |
| M7 | [09](09-compatibility.md) §3 双向版本矩阵 | **未开始。** 需要先发一个版本才有 golden 二进制可入库 |

**M3 必须在 M5 之前完成。** 屏障和 CI 矩阵是后续所有接口的安全网，补完接口再回头加验证的代价高得多。

M5 的六张接口表按依赖顺序实现：`mcdk.core` → `mcdk.console` → `mcdk.info` → `mcdk.log` → `mcdk.game` → `mcdk.mcp`。`mcdk.mcp` 排最后，因为它的 handler 通常要调用 `mcdk.game`，先把被依赖方做完才好写 example。

### 1.1 M4 实现纪要

总线：订阅计数数组、SYNC/QUEUED/MAIN 三种派发、惰性派发线程、有界队列、槽位复用的订阅表、
detach 等 in-flight。九个事件全部接上发射点。

实现过程中被抓出并修正的四个缺陷，在此留档以免重踈：

1. **QUEUED 的「深拷贝」曾经是一次 `memcpy`。** `mcdk_str` 里只有指针被复制，指向的字节没有——
   发射方作用域一结束就是 use-after-free。它当时能跦着测试，因为测试只断言了整数字段，
   而释放掉的内存往往还留着原字节。现在 payload 连同字符串字节打成自包含 blob，指针位先存偏移、
   落到最终地址后再还原。
2. **`MCDK_DISPATCH_MAIN` 当时完全不工作。** 它会在派发线程上被调用（等于退化成 QUEUED），
   而且只有 MAIN 订阅者时派发线程压根不会被创建，事件堆满后静静丢弃。
3. **无异步订阅者时仍无条件入队。** 纯 SYNC 订阅的场景下队列会涨满 4096，然后报出一句
   「某个插件的处理器过慢」的假告警。
4. **为了抽干 `post_main` 而把主线程改成定时轮询，违反了零插件开销契约。**

第 2 与第 4 条后来一并作废：主线程派发整个被移除了（[04-events.md](04-events.md) §4.0）。
一个坏了这么久都没人察觉的功能，本身就是它没有用户的证据。

## 2. ABI 冻结点

`abi/core.h` 与 `abi/entry.h` 在 M2 结束时冻结，此后只能按 [02-abi-contract.md](02-abi-contract.md) §8 的规则追加。各 `iface/*.h` 在其接口首次随版本发布时冻结。

冻结之前是唯一可以自由改动 ABI 的窗口，**[02-abi-contract.md](02-abi-contract.md) 的集中评审应当在 M2 开始前完成**。

## 3. 为进程外插件预留

所有接口的首参数均为 `mcdk_handle self`，且所有数据都是可序列化的 POD 或 UTF-8 串——这两条使得同一套接口形状可以在将来换成跨进程传输，实现基于现有 MCDevLink（asio）。

进程内插件的崩溃隔离问题最终只能靠这条路解决，因此**禁止**在接口中引入无法序列化的参数（裸内存指针、回调之外的函数指针）。

## 4. 待定问题

1. **`.mcdev.json` 写回策略。** `mcdk plugin add/enable/disable` 会改写配置文件，需与现有 `tryUpdateUserGamePath` 共用一套保留注释与字段顺序的读改写实现。当前 `.mcdev.json` 含 jsonc 风格注释的可能性需确认。
2. **事件总线线程数。** 单线程串行（简单、可预测、插件间有序）还是按插件分线程（隔离性好、复杂、丢失全局顺序）。倾向单线程，待 M4 压测决定。
3. **`patch_user_config` 的字段白名单。** 全量开放会让多插件冲突难以排查。倾向白名单，初版建议只开放 `modDirectories`、`hotReload`、`windowStyle`、`debugOptions`。
4. **大数据返回通道。** `mcdk.game` 的窗口截图单帧可达数 MB，是否单列共享内存通道而非走宿主分配器。
5. **非 Windows 平台时间表。** ABI 设计已平台中立，但 `mcdk.game` 的窗口能力是 Windows 专有，其在其他平台返回 `MCDK_ERR_NOT_SUPPORTED` 的约定已定，实际移植时间待议。
6. **用户级全局插件声明。** v1 只有项目级 `.mcdev.json`（见 [06-loading.md](06-loading.md) §6）。"我的插件在所有项目生效"是可预见的需求，但引入全局列表时**必须**保持显式声明这一性质，不得退化为目录扫描。
7. **插件二进制的调试体验。** 插件由用户自行构建，pdb 与源码路径不在 mcdk 侧。是否需要在 `mcdk plugin list` 中展示符号信息以便排查。
8. **是否在 `.clang-tidy` 启用 `readability-identifier-naming`。** [11-code-style.md](11-code-style.md) 归纳的命名约定目前靠自觉。启用后会同时对存量代码生效（已知至少 `NativeBridgeLoader::impl_` 与若干子命名空间会报警），需先评估改动面。
9. **`mcdk.game` 是否在 v1 加入 `env_set`。** 决定 `mcdk.game.launch.before` 能否修改子进程环境变量，见 [04-events.md](04-events.md) §4.1。实现成本低，取决于是否有真实用例。
