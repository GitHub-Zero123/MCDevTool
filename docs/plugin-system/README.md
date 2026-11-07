# MCDK 插件系统

- 状态：Draft v1，设计阶段，尚未实现
- 目标仓库：`MCDevTool`
- 参考实现：Godot 4.x GDExtension（`core/extension/gdextension_interface.h` @ 4.4-stable，godot-cpp master）
- 最后更新：2026-09-21

本目录下的文档共同构成插件系统的完整设计。文中的"必须""禁止""应该""可以"分别对应 MUST、MUST NOT、SHOULD、MAY。凡标记为**规范**的条目，实现和评审都必须逐条对照。

## 文档索引

| 文档 | 内容 | 谁该读 |
| --- | --- | --- |
| [01-overview.md](01-overview.md) | 背景、目标与非目标、三层架构、仓库目录划分 | 所有人 |
| [02-abi-contract.md](02-abi-contract.md) | **ABI 兼容性契约（规范）** | 宿主与 SDK 实现者，必读 |
| [03-abi-reference.md](03-abi-reference.md) | ABI 定义：基础类型、入口点、接口查询、生命周期阶段 | 宿主与 SDK 实现者 |
| [04-events.md](04-events.md) | 事件系统：线程模型、派发模式、事件清单 | 宿主实现者、插件作者 |
| [05-interfaces.md](05-interfaces.md) | **v1 接口定义**：MCP 工具注册 / 控制台 / Python 执行 / 会话信息 / 截图 / 日志缓冲 | 宿主实现者、插件作者 |
| [06-loading.md](06-loading.md) | `.mcdev.json` 声明、`plugin.json` 清单与加载流程 | 宿主实现者、插件作者 |
| [07-sdk.md](07-sdk.md) | 用户侧 SDK 形态与 CMake 集成 | 插件作者 |
| [08-host-integration.md](08-host-integration.md) | 宿主侧集成点与两项前置重构 | 宿主实现者 |
| [09-compatibility.md](09-compatibility.md) | 兼容性验证：编译器矩阵、一致性套件、CI 静态检查 | 宿主实现者、CI 维护者 |
| [10-roadmap.md](10-roadmap.md) | 实施里程碑、ABI 冻结点、待定问题 | 所有人 |
| [11-code-style.md](11-code-style.md) | 命名风格：C ABI 层与 C++ 层的分界及理由 | 宿主与 SDK 实现者 |
| [12-performance.md](12-performance.md) | **零插件开销契约**、发射点标准写法、性能回归基准 | 宿主实现者，必读 |
| [13-registry.md](13-registry.md) | **ABI 与事件登记表**，新增一项的强制流程 | 每个改动 ABI 的人 |

## 一句话概括

插件以动态库分发，宿主与插件之间只有**纯 C ABI**；两侧各自的 C++ 特性（异常、STL、继承、RAII）都在自己的边界内消化掉，因此插件**可以由任意编译器、任意标准库、任意 CRT 构建**，且该性质由 CI 矩阵持续验证而非靠文档约定。

## v1 范围

六项业务能力：注册 MCP 工具、线程安全的彩色控制台输出、Python 代码执行、会话信息查询（游戏路径 / MCP 端口 / 游戏 IPC 端口等）、屏幕捕获、读取游戏日志缓冲区。加上 `mcdk.core` 与 `mcdk.events` 两项基础设施，共 6 张接口表、9 个事件。

插件由用户在 `.mcdev.json` 中逐条显式声明，宿主**不扫描任何目录**。

范围之外的能力（配置读写、Pack 干预、自定义热更新 watcher、Host Bridge RPC 等）已留档但明确推迟，见 [05-interfaces.md](05-interfaces.md) §10。

## 两条贯穿开发期的硬约束

1. **零插件开销**（[12-performance.md](12-performance.md)）：未装插件时，每个事件发射点的开销不得超过一次 relaxed 原子读加一次可预测分支。由 CI 基准测试守护，不是口头目标。
2. **改 ABI 必登记**（[13-registry.md](13-registry.md)）：新增或弃用任何 ABI 函数、接口表、事件，都必须更新登记表，CI 与头文件注释双向校验，缺登记的 PR 拒绝合并。

## 阅读顺序建议

第一次读：[01](01-overview.md) → [02](02-abi-contract.md) → [04](04-events.md) → [10](10-roadmap.md)。

动手实现前补读：[12](12-performance.md) 与 [13](13-registry.md)。

[02-abi-contract.md](02-abi-contract.md) 是整套设计的核心，其余文档的所有接口定义都必须服从它。该文档一旦随 M2 冻结（见 [10-roadmap.md](10-roadmap.md)），后续只能按其 §7 的规则追加，因此**值得在实现开始前集中评审**。
