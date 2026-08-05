# `mc_profiler` MCP 工具实施计划

- 状态：已实现；Debug/Release 离线构建与测试通过，真实游戏 E2E 待验证
- 目标仓库：`MCDevTool`
- 参考实现：`D:\Zero123\CPP\mcdev-tools`
- 最后更新：2026-08-05

## 1. 目标

将 VS Code 插件中的以下能力迁移到 MCDK 后端，并通过一个 MCP 工具开放：

- Python CPU/WALL 性能分析，支持 `client`、`server`、`all`。
- Python 内存增量分析，基于 `tracemalloc`。
- Native CPU 性能分析，基于与游戏协议匹配的 Tracy Server 0.11.1。
- 面向 Agent 的服务端筛选、分级召回、自动超时和受控落盘。
- 面向未来业务的可直接调用后端 API，不要求调用方经过 MCP 网络协议。

MCP 工具注册名固定为：

```text
mc_profiler
```

Agent 通过结构化 `op + args` 调用。不能依赖 Agent 主动停止采集、控制返回量或清理资源，这些约束必须由后端强制实施。

## 2. 核心设计原则

### 2.1 协议与业务分离

MCP 只是适配层，不是 profiler 业务实现层。

```text
MCP / CLI / Host Bridge / future service
                 |
          protocol adapters
                 |
          ProfilerService API
                 |
     jobs / backends / store / reports
```

约束：

- 核心库不得依赖 `cpp-mcp`。
- 核心 API 不接收 MCP `op` 字符串，不返回 MCP `content` 或 `structuredContent`。
- 核心错误使用稳定 domain error，由各协议适配器映射成 MCP、CLI 或其他协议错误。
- 核心任务不得依赖 MCP session 生命周期；MCP client 断开后任务仍按 deadline 自主收尾。Python 必须完成清理和落盘；Native 超过收尾时限时按 10.1 进入 `cleanup_pending`。
- MCP adapter 只负责参数反序列化、调用 typed API、返回 envelope。
- 每个游戏 runtime 只能由组合根创建一个 `ProfilerRuntimeOwner`；MCP 和其他业务通过它取得同一个 `ProfilerService` 共享引用，不建立本机 MCP 网络回环，也不得自行创建第二个绑定同一游戏进程的 service。

### 2.2 惰性初始化

- 没有 profiler 调用时不创建任务线程、不扫描端口、不加载 DLL、不扫描历史目录。
- 首个 profiler op（包括 `/help`、`/guide` 和参数错误请求）惰性构造共享 service，并探测、校验和尝试加载固定位置的 Native DLL，使帮助响应能反映实际 capability。
- DLL 探测不等于 endpoint discovery；只有 Native start 或 `kind=native.cpu, deep=true` 的 doctor 才枚举端口。
- Python 调用会复用已构造的 service，但不扫描 Tracy endpoint、不创建 Native capture worker。
- history 目录扫描和报告导出仍分别延迟到对应 API 首次调用。

### 2.3 AI 不可信原则

- 提示词只能改善行为，不能作为正确性或资源释放边界。
- 所有 start 必须有有限 deadline。
- 所有 query 必须有条数和序列化字节双重上限。
- 所有输出路径由服务端决定。
- 所有 profiler 默认全局互斥，防止并行采集污染测量结果。

## 3. 非目标

- 不迁移 VS Code Webview UI。
- 不让 Agent 提供任意 DLL 路径或报告输出路径。
- 不让 Agent 触发在线下载或编译第三方组件。
- 第一版不支持多个 profiler 并行运行。
- 第一版不通过 MCP 返回完整 `.tracy` 文件、完整调用树或无界结果集。
- 第一版不承诺稳定的跨 DLL C++ ABI；先提供仓库内可复用的 typed C++ API。

## 4. 已确认的现有约束

### 4.1 Python CPU

参考：

- `D:\Zero123\CPP\mcdev-tools\src\hostBridge\pythonProfiler.ts`
- `D:\Zero123\CPP\mcdev-tools\src\hostBridge\pythonProfilerController.ts`

现有行为：

- 使用 Yappi，支持 `CPU`、`WALL`。
- 支持 `client`、`server`、`all`。
- `all` 需要分别在客户端和服务端线程执行 marker。
- 当前结果最多保留 160 个函数和 480 条调用边。
- 游戏内 timer 只停止 Yappi，最终 collect 由 VS Code controller 触发。

### 4.2 Python memory

参考：

- `D:\Zero123\CPP\mcdev-tools\src\hostBridge\pythonMemoryProfiler.ts`
- `D:\Zero123\CPP\mcdev-tools\src\hostBridge\pythonMemoryProfilerController.ts`

现有行为：

- 使用 `tracemalloc` 快照差值，仅在 client 侧执行。
- traceback depth 范围 1 到 16，默认 8。
- 当前最多保留 80 个 allocation site。
- 当前是开放式 start/collect，迁移后必须由 MCDK watchdog 自动 collect。

### 4.3 Native

参考：

- `D:\Zero123\CPP\mcdev-tools\native\tracy-bridge`
- `D:\Zero123\CPP\mcdev-tools\src\hostBridge\nativeProfilerCapture.ts`

现有行为：

- 仅支持 Windows x64。
- Tracy Server 必须精确匹配游戏使用的 0.11.1。
- bridge 使用 Tracy、Capstone 和 Tracy 所需压缩源码。
- bridge C ABI 版本为 1。
- Tracy 有进程级状态，当前只允许一个活动 capture。
- Native 分析 JSON 最大 16 MiB；这不是适合 Agent 的返回量。
- 当前 Release DLL 约 3.7 MB。

### 4.4 MCDK

参考：

- `tools/mcdk/src/mcp_server.cpp`
- `tools/mcdk/src/game_process.cpp`
- `tools/mcdk/src/ipc_code_execution.cpp`

现有行为：

- MCP Server 与游戏 IPC 位于同一个 MCDK 进程。
- Python profiler 应直接复用游戏 IPC，不通过 VS Code Host Bridge 回环。
- 当前 MCP `CodeExecuteHandler` 生成 MCP 文本，不适合作为内部结构化执行 API。
- `cpp-mcp` 已能转发 `structuredContent`。

## 5. 已实现模块结构

```text
mcdev_profiler_core                 # 不依赖 MCP
  ProfilerRuntimeOwner              # 每个游戏 runtime 唯一所有者
  ProfilerServiceProvider           # 惰性取得共享 service
  ProfilerService                   # typed API
  DefaultProfilerService            # job/backend/store/report 的进程内实现
  NativeBridgeLoader                # C ABI 的 C++ wrapper

mcdk_core
  mc_profiler_mcp                   # op 校验、help/guide、domain-to-MCP 映射

mcdk_runtime/game_process
  GameCodeExecutor adapter
  current game PID/process context

mcdev-tracy-bridge.dll
  Tracy 0.11.1 server implementation
  stable C ABI v1
```

实际文件：

```text
tools/mcdk/include/performance/
  profiler_runtime_owner.hpp
  profiler_service.hpp
  profiler_service_factory.hpp
  profiler_types.hpp
  native_bridge_loader.hpp

tools/mcdk/src/performance/
  profiler_service.cpp
  profiler_runtime_owner.cpp
  profiler_types.cpp
  native_bridge_loader.cpp

tools/mcdk/src/
  mc_profiler_mcp.cpp

components/profiler/tracy-bridge/
  include/mcdev_tracy_bridge.h
  src/api.cpp
  src/capture_session.cpp
  src/result_builder.cpp
```

## 6. 后端直接调用 API

示意接口：

```cpp
class ProfilerService {
public:
    std::expected<JobSnapshot, ProfilerError> start(const StartRequest& request);
    std::expected<JobSnapshot, ProfilerError> status(const JobId& id) const;
    std::expected<JobSnapshot, ProfilerError> stop(const JobId& id);
    std::expected<void, ProfilerError> discard(const JobId& id);
    std::expected<QueryPage, ProfilerError> query(const QueryRequest& request) const;
    std::expected<DetailResult, ProfilerError> detail(const DetailRequest& request) const;
    std::expected<HistoryPage, ProfilerError> history(const HistoryRequest& request) const;
    std::expected<ExportResult, ProfilerError> exportReport(const ExportRequest& request);
    std::expected<CleanupResult, ProfilerError> cleanup(const CleanupRequest& request);
    std::expected<Capabilities, ProfilerError> inspectCapabilities(const DoctorRequest& request);
};
```

要求：

- Request/Result 是 typed domain object，不是 MCP JSON envelope。
- `ProfilerService` 不由普通业务直接构造；`ProfilerRuntimeOwner` 绑定唯一 `IProcessContext`，并通过 `ProfilerServiceProvider` 惰性发布共享实例。
- MCP adapter、未来 Host Bridge/CLI adapter 和进程内业务必须使用同一个 provider，保证活动任务锁、Native module、结果仓库和 shutdown 顺序只有一份。
- `start` 只在采集器确认启动且 watchdog 已安装后返回。
- 异步任务由 service 自己拥有，不借用 adapter 生命周期。
- 可注入 `IGameCodeExecutor`、`IClock`、`IResultStore`、`IProcessContext`，方便其他业务和测试复用。
- 查询 API 直接表达 filter/sort/cursor，不复用 MCP `/query` 文本解析。
- MCP adapter 将 `op` 映射为上述方法，未知字段和类型在进入 service 前拒绝。

### 6.1 结构化游戏执行接口

从现有 MCP handler 中抽出：

```cpp
class IGameCodeExecutor {
public:
    virtual ~IGameCodeExecutor() = default;
    virtual std::expected<nlohmann::json, GameExecutionError> execute(
        std::string code,
        GameSide side,
        std::chrono::milliseconds timeout
    ) = 0;
};
```

- 返回游戏 `return_value` 解包后的 JSON。
- 保留 side、timeout、game error code 和 retryable。
- `execute_code` MCP 工具和 profiler 共用 executor。
- executor 不生成 MCP `content`。

## 7. 惰性生命周期

| 触发点 | 允许初始化 |
|---|---|
| MCDK 启动 | 注册 `mc_profiler` adapter；不构造 service |
| 首个 profiler op（含 `/help`、`/guide`） | 共享 service、job manager、固定路径 Native DLL probe/load；不扫描端口 |
| `/doctor` | 复用首次 probe 结果；仅显式 `kind=native.cpu, deep=true` 执行 endpoint discovery |
| 首次 Python start | 指定 Python backend；不执行 endpoint discovery |
| 首次显式 Native runtime request | Native start 执行 endpoint discovery 并创建 capture worker；Native deep doctor 只执行 discovery |
| query/detail | 指定 job 的摘要或索引 |
| history | 此时才扫描 manifests |
| export | 指定报告生成器 |

实现约束：

- `ProfilerServiceProvider` 首次构造由 mutex 和 condition variable 保护，失败可重试。
- Native loader 使用可重试状态机；DLL 缺失后安装组件可再次探测。
- DLL 成功加载后保留到 MCDK 退出，不在任务间反复 `FreeLibrary`。
- Native deep doctor 是有副作用的显式诊断：它会扫描端口；DLL 已由首个 profiler op 探测，成功后常驻到 MCDK 退出。普通 `/doctor` 不扫描端口。
- service 析构先停止任务并 join worker，最后释放 backend/module。
- 不使用后台 Tracy endpoint 扫描；Native start 或 Native deep doctor 每次请求时扫描一次，活动 capture 内只轮询 capture 状态。

## 8. MCP 协议

### 8.1 Input Schema

```json
{
  "type": "object",
  "required": ["op"],
  "properties": {
    "op": {"type": "string"},
    "args": {"type": "object"}
  },
  "additionalProperties": false
}
```

单工具包含读写操作，tool annotation 必须保守标记为非只读。每个 op 由 adapter 独立严格校验。

固定 annotations：

```text
readOnlyHint=false
destructiveHint=true
idempotentHint=false
openWorldHint=true
```

这些只是 MCP 客户端提示，不代替权限和参数校验。`args` 的合法字段由每个 op 的 typed decoder 以 `additionalProperties=false` 语义验证。

当前 `tool_builder` 不能完整表达顶层 `additionalProperties=false` 和各 op 的条件 schema，因此 `buildMcProfilerTool()` 应直接构造规范 `parameters_schema`，或先扩展通用 builder；不得发布一个宽松 schema 后只在文档中声称严格。

### 8.2 操作

| op | 后端 API |
|---|---|
| `/help` | adapter 静态帮助 |
| `/guide` | adapter 静态 playbook |
| `/doctor` | `inspectCapabilities` |
| `/start` | `start` |
| `/status` | `status` |
| `/stop` | `stop` |
| `/query` | `query` |
| `/detail` | `detail` |
| `/history` | `history` |
| `/export` | `exportReport` |
| `/discard` | `discard` |
| `/cleanup` | `cleanup` |

Profiler kind：

```text
python.cpu
python.memory
native.cpu
```

`/doctor` 的 `deep=true` 只有在同时指定 `kind=native.cpu` 时合法；该组合属于显式 Native runtime request，而不是只读静态检查。其他 kind 的深度诊断需要后续单独定义，禁止复用这一语义进行隐式初始化。

示例：

```json
{
  "op": "/start",
  "args": {
    "kind": "python.cpu",
    "target": "client",
    "clock": "wall",
    "duration_seconds": 15
  }
}
```

### 8.3 返回

规范结果放入 `structuredContent`。`content` 仅放一句兼容摘要和 job id，不复制完整 JSON。

所有 op 共用一个稳定 output envelope；op 专属数据放入 `data`：

```json
{
  "type": "object",
  "required": ["ok", "op", "next_calls"],
  "properties": {
    "ok": {"type": "boolean"},
    "op": {"type": "string"},
    "job": {"type": ["object", "null"]},
    "data": {"type": ["object", "array", "null"]},
    "error": {"type": ["object", "null"]},
    "warnings": {"type": "array", "items": {"type": "object"}},
    "next_calls": {"type": "array", "items": {"type": "object"}}
  },
  "additionalProperties": false
}
```

`cpp-mcp` 当前只发布 `outputSchema` 并转发 `structuredContent`，不会执行 schema validation。因此 adapter 在返回前必须运行 `validateProfilerEnvelope()`；contract tests 对内置 MCP 和 stdio bridge 的本地 help/guide 使用同一验证器。

Domain error 映射为稳定结构：

```json
{
  "ok": false,
  "op": "/start",
  "error": {
    "code": "PROFILER_BUSY",
    "message": "Another profiler job is active.",
    "retryable": true,
    "details": {}
  },
  "next_calls": []
}
```

每个响应最多返回 3 个上下文相关的 `next_calls`。

### 8.4 `mcdk_stdio_bridge` 暴露

`mcdk_stdio_bridge` 必须在游戏尚未启动时通过 `tools/list` 暴露 `mc_profiler`，保持其作为稳定 stdio MCP 入口的现有语义。

实现方式：

- 在共享 `mcdk::mcp_tool_definitions` 中增加 `buildMcProfilerTool()`。
- 将 `buildMcProfilerTool()` 加入 `buildAllTools()`。
- MCDK 内置 MCP 和 `mcdk_stdio_bridge` 继续从同一个定义生成 `tools/list`，禁止各维护一份 schema/description。
- 工具定义包含 `op` string、可选 `args` object、output schema 和保守的非只读 annotations。
- description 明确提示使用 `/help`、所有 start 自动超时、结果默认分级召回。

stdio bridge 的职责边界：

- 不构造 `ProfilerService`。
- 不链接或加载 `mcdev-tracy-bridge.dll`。
- 不启动 watchdog、扫描游戏进程、扫描 Tracy endpoint 或访问报告目录。
- `/doctor`、`/start`、`/status`、`/stop`、`/query`、`/detail`、`/history`、`/export`、`/discard`、`/cleanup` 原样转发给游戏内 MCDK MCP。
- 后端未启动时继续返回明确 tool error，不在 stdio bridge 内创建替代任务。

帮助回退：

- `/help` 和 `/guide` 先尝试转发，使 MCDK 的首次 op 初始化和 DLL probe 能影响帮助中的 runtime capability。
- MCDK 不可达时，stdio bridge 才使用共享静态帮助作为回退，并明确标记 `runtime.status=unavailable`，不得猜测 DLL 状态。
- 本地响应必须调用与 MCDK MCP 共用的 `tryBuildLocalResult()`，禁止复制帮助文本。
- 本地 help/guide 与内置 MCP 共用 envelope 构造和 `validateProfilerEnvelope()`，不能只共享文本后分别拼装返回结构。
- stdio bridge 自身不得构造 service、加载 DLL 或检查运行环境。
- `/doctor` 必须转发，因为平台、DLL、游戏 IPC、project 和 Tracy endpoint 都属于实际 MCDK 实例状态。

版本一致性：

- Release 必须将匹配版本的 `mcdk` 与 `mcdk_stdio_bridge` 一起打包。
- bridge 本地定义比远端 MCDK 新时，远端可能不认识 `mc_profiler`；bridge 应把远端 method/tool not found 转换为明确的 `BACKEND_TOOL_UNAVAILABLE`，提示检查版本，而不是伪装成 profiler 参数错误。
- 不为解决版本不一致而在每次 `tools/list` 时连接后端，否则会破坏离线暴露和惰性连接行为。

## 9. 帮助与启发

### 9.1 `/help`

- `/help`：短命令索引、安全约束、可用 guide。
- `/help {op}`：参数、边界、完整 JSON 示例。
- `/help {kind}`：前提、测量干扰、可查询 view。
- `/help native.cpu` 必须说明 Native 返回的是按线程组织的 Tracy zone 调用层级：当游戏在同一路径同时发布 Python-facing 与 C++ zones 时，可以跨语言追踪父子耗时，并定位数驱 JSON 解析/反序列化、属性转换、对象构建、事件分发等已埋点底层阶段。
- Native help 必须同时说明证据边界：这不是无条件的 OS sampled machine stack；未埋点的 native 工作、目标构建未提供的名称/源码位置不会凭空出现。Agent 必须检查 index/call-tree coverage 与 truncation，缺失 zone 只能视为缺失证据。
- Native 判读提示必须区分 inclusive total 与 self：高 total/低 self 应继续向子节点下钻，高 self 才说明耗时主要发生在当前 zone；结合 calls、mean、maximum 区分累计开销与单次尖峰。
- 明确说明采集会在 deadline 自动请求停止，不要求 Agent 保持连接或主动 stop；同时说明 Native 收尾超时可能进入 `cleanup_pending`，不能承诺固定时间内销毁同进程 worker。

### 9.2 `/guide`

第一版：

```text
lag
python-hotspot
memory-growth
native-hotspot
compare-before-after
```

每个 guide 提供适用条件、不适用条件、推荐 kind、参数范围、干扰风险、结构化调用步骤和结果判读方法。提示不得替代服务端 deadline、互斥、返回量和路径限制。

## 10. 任务状态机和自动关闭

```text
created -> starting -> running -> finalizing -> persisting -> completed
                 |          |            |             |
                 +----------+------------+-------------+-> failed
                                           +------------> cleanup_pending -> failed
                                           +------------> discarded
                                           +------------> aborted
```

规则：

- `duration_seconds` 必须有限；各 kind 有服务端最小、默认、最大值。
- deadline 使用 `steady_clock`；展示时间使用 `system_clock`。
- status、query、MCP 重连不得续期。
- `/stop` 幂等，含义固定为提前 finalize 并保留结果。
- `/discard` 表示中止并删除，不能与 stop 混淆。
- 第一版所有 kind 共用一个活动任务锁。
- start 部分失败必须执行 backend cleanup。
- 游戏退出或 MCDK shutdown 时不得遗留 detached worker。

三层关闭：

1. backend 内部截止：Yappi timer 或 Tracy `maximumSeconds`。
2. MCDK watchdog：soft deadline 到达后主动 collect、cleanup、persist。
3. completion deadline：finalize 超过 grace 后拒绝新的 `/start`，但继续允许 `/status` 和诊断查询；状态进入 `cleanup_pending`，继续持有全局任务锁并等待 cooperative cleanup 完成。

Python memory 必须新增 host watchdog，禁止保留开放式 start/collect 模式。

### 10.1 Native 超时的真实边界

Native bridge 当前在同一 MCDK 进程内运行，`release()` 最终需要 join Tracy worker。C++ 不能安全强杀同进程线程，因此第一版不得承诺“到达某个 wall-clock 时间后 worker 必然已经销毁”。

第一版语义：

- Tracy `maximumSeconds` 限制采集阶段。
- MCDK soft deadline 请求 stop。
- finalizing 超过 grace 后 job 进入 `cleanup_pending`，向调用方报告 `NATIVE_FINALIZE_TIMEOUT`，但不能发布为已清理的终态。
- `cleanup_pending` 期间保持全局 profiler 锁，不允许启动下一次采集。
- bridge worker 结束后执行 release/join，最终转为 `failed` 并记录实际 cleanup 时间。
- MCDK shutdown 仍需 cooperative join，禁止 detach 或卸载仍有活动线程的 DLL。

如果产品要求“即使 Tracy 内部卡死，MCDK 也必须在固定时间内退出”，则必须把 Tracy Server 放入独立 helper process，并由 MCDK 在超时后终止该进程；DLL 线程模型无法提供这一强保证。helper process 不属于第一版范围，但必须作为严格 hard-kill 模式的唯一升级路径记录，而不是以后尝试强杀线程。

## 11. 数据召回和服务端分拣

### 11.1 分层

| 层级 | 数据 | 默认返回 Agent |
|---|---|---:|
| L0 | summary、关键指标、覆盖和截断 | 是 |
| L1 | 服务端排序后的 Top-K | 按需 |
| L2 | 稳定 ID 的局部详情和邻域 | 按需 |
| L3 | 完整的已持久化有界数据集、Markdown、SVG、`.tracy` | 只落盘；文本使用 UTF-8，返回服务端生成的规范化绝对路径 |

### 11.2 Views

- Python CPU：`hotspots`、`functions`、`callers`、`callees`、`contexts`。
- Python memory：`allocations`、`growth`、`retained`、`traceback`。
- Native：`hotspots`、`threads`、`calltree-roots`、`calltree-children`、`source-locations`。

### 11.3 强制边界

- 默认 limit 初步为 20，服务端最大值初步为 50，最终值通过测试校准。
- 同时限制记录数和序列化字节数。
- 单次 Agent 返回目标不超过约 64 KiB，最终阈值配置化。
- 调用树只返回指定节点的直接 children。
- 使用稳定 `function_id`、`node_id`、`allocation_id`。
- 返回 `total_available`、`returned`、`truncated`、`next_cursor`。
- cursor 绑定 job、view、filter、sort、order；参数变化后失效。
- sort/filter 在服务端执行。
- 原始值和单位分开，不返回重复格式化字段。

### 11.4 采集层数据契约

召回上限与采集上限是两层约束。当前实现采用“游戏侧单次有界快照 + MCDK 本地持久化和分页查询”：

1. soft deadline 到达后先停止 Yappi/tracemalloc 采集。
2. 游戏侧只保留项目相关记录，CPU 最多返回 512 个函数和 2048 条调用边，memory 最多返回 512 个 allocation site；traceback depth 最大 16，符号与路径字符串也有长度上限。
3. 游戏侧响应显式包含 observed total 与 `truncated`，MCDK 不把采集阶段已截断的数据伪装成完整全集。
4. MCDK 持久化该 job 的完整有界快照；Agent 后续只通过服务端 filter/sort/cursor 分页召回，单次最多 50 条且约 64 KiB。
5. 成功、失败、提前 stop、discard 和 shutdown 均走幂等 cleanup；游戏侧另有 duration + 60 秒 hard TTL，即使 Agent 或 MCDK 不再调用也会释放 profiler 状态。

采集预算：

- Python CPU 分别限制 functions、call edges、符号长度和持久化数据规模。
- Python memory 分别限制 allocation sites、traceback depth、路径长度和持久化数据规模。
- 达到采集预算时停止追加并记录 `capture_truncated=true`、`captured_records`、`total_observed` 和 truncation reason。
- 采集预算由服务端配置决定，Agent 不能请求无界值或绕过上限。
- stable ID 在快照生成时确定，后续落盘、分页和 `/detail` 不重新编号。

Native 数据契约不同：

- `.tracy` 是完整 trace artifact，Native JSON index 是按服务端 capture budget 生成的有界分析索引。
- `/query` 只查询已持久化索引，不能声称可分页召回索引生成时未保留的所有 zone。
- 返回 `index_truncated`、`indexed_zones`、`total_zones` 和 call-tree coverage。
- 第一版不为了任意查询而重复解析 `.tracy`；需要扩大覆盖时调整服务端预算并重新采集。

因此，本文中的“完整数据”统一表示“该 job 已持久化的完整有界数据集”，不表示进程运行期间产生的无限全集。扩大 Python 采集覆盖需要重新采集或后续引入多页 IPC snapshot 协议；当前实现不会假装能召回采集时未保留的记录。

## 12. 受控落盘

默认目录：

```text
<project>/.mcdev/profiles/<job-id>/
  manifest.json
  summary.json
  data.json
  report.md
  report.svg
  capture.tracy
```

规则：

- 无 project root 时使用 MCDK 配置的受控数据目录，不回退到当前工作目录。
- job id 由服务端生成，目录名不接受调用方输入；创建前固定并规范化 storage root。
- 先写同目录临时文件，再执行同卷原子 rename；目标文件必须不存在，不使用覆盖式 rename。
- 提交顺序固定为 `data/index -> summary -> manifest`，`manifest.json` 最后 rename，作为 job 完成的 commit record。
- 每个临时文件必须完成 write、flush、close 和错误检查。若要求断电级恢复，Windows 实现还需对文件和父目录使用相应 handle 执行 `FlushFileBuffers`；仅 `ostream.flush()` 不能声明为 durable。
- 必须文件持久化成功后才能发布 `completed`。Markdown、SVG 属于可重建 export，不作为 capture completed 的前置条件。
- Agent 只能选 export 格式，不能指定任意路径。
- 路径执行 canonical/containment 校验，拒绝 storage root 下非服务端创建的 reparse-point/symlink job 目录；仅字符串前缀比较不足以防止链接逃逸和检查后替换竞态。
- 返回 artifact path 时统一转换为 UTF-8 generic absolute path，并同时返回 artifact kind/size/hash；Agent 不需要也不能补全相对路径。
- Native 临时 trace 复制到受控目录并验证非空后才删除临时目录。
- retention 同时限制 job 数、总字节数、TTL 和单个 trace 大小。
- 具体配额用真实 trace 样本确定，不能凭估计固化。
- 首次 history 或首次写入时才扫描并恢复遗留 manifest。
- 恢复时只有完整、可解析且引用 artifact 均通过校验的最终 manifest 才视为 committed；残留 `.tmp` 和无 manifest 目录按有界清理策略处理。

## 13. Native DLL 决策

采用：

```text
源码单一权威
+ 独立 DLL
+ MCDK target 可依赖并联动构建 DLL
+ 运行时首次 Native 调用才加载
+ 发行包是否携带、用户是否下载由发行策略决定
```

不采用：

- Tracy 静态链接进 `mcdk.exe`。
- Agent 触发在线下载或编译。
- 从 `PATH`、当前目录搜索同名 DLL。
- MCDK 和 VS Code 仓库各自维护一份 bridge 源码。

### 13.1 组件布局

```text
mcdk.exe
native-profiler/
  component.json
  mcdev-tracy-bridge.dll
  licenses/
    LICENSE-Tracy.txt
    LICENSE-Capstone.txt
```

`component.json`：

```json
{
  "component": "native-profiler",
  "bridge_api": 1,
  "tracy_protocol": "0.11.1",
  "platform": "windows",
  "arch": "x64",
  "sha256": "<release hash>"
}
```

### 13.2 安全加载

- 只从安装目录或受控组件根目录的固定相对位置加载。
- 使用绝对路径以及 `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32` 调用 `LoadLibraryExW`，避免当前目录和 `PATH` 搜索劫持。
- 校验 manifest、文件 hash、C ABI version 和 Tracy protocol。相邻 manifest 中的 SHA-256 只用于检测文件损坏和版本错配，不是发布者身份的信任根。
- 如果威胁模型包含安装目录被篡改，发行组件必须使用 Authenticode，或使用内置可信公钥验证签名 manifest；不能依赖攻击者可同时替换的 DLL 与相邻 hash 文件。
- 所有函数指针解析完成后才发布 module ready。
- Agent 不得传 DLL 路径或关闭验证。
- DLL 缺失只令 `native.cpu` unavailable，Python profiler 继续可用。

### 13.3 ABI 边界决策

结论：DLL 对外保留窄 C ABI，MCDK 内部在 C ABI 之上提供 typed RAII C++ wrapper；不直接导出 C++ class。

“MCDK 和 DLL 使用同一编译器”只能说明当前组合大概率可工作，不能单独保证长期 C++ ABI 稳定。以下条件仍必须完全一致：

- MSVC toolset 和 STL 实现版本。
- `/MT`、`/MTd`、`/MD`、`/MDd` CRT 模式。
- `_ITERATOR_DEBUG_LEVEL`、Debug/Release 和 sanitizer 配置。
- exception、RTTI、结构体 packing、编译开关。
- 对象由哪一侧分配、释放以及 DLL 卸载时机。

本项目还允许 Native component 与 core 分开打包、补装和升级，因此未来不一定始终是同一次编译产生的严格配对产物。C++ ABI 会把可选组件变成与特定 MCDK 二进制强绑定的私有插件，收益有限，风险高于 C ABI。

保留现有 C ABI 形态：

- opaque `mcdev_tracy_handle`。
- 固定宽度整数和 UTF-8 `char*`。
- 调用方分配 result/error buffer，DLL 不把内存所有权交给调用方。
- JSON 作为结果边界，不暴露 Tracy、STL、thread 或 mutex 类型。
- `api_version` 和 `protocol_version` 独立校验。

MCDK 侧已实现封装：

```cpp
class NativeBridgeLoader;  // LoadLibrary/GetProcAddress、版本校验和全部 C ABI 调用
struct NativeCaptureHandle;
```

业务代码只通过 `NativeBridgeLoader` 使用 handle；service 在成功、失败、discard 和 shutdown 路径显式 stop/release，loader 析构执行全局 shutdown。函数指针不散布到业务层，也不把 C++ ABI 暴露到 DLL 边界。

C ABI 必须补强：

- 所有导出函数都必须是异常边界，任何 C++ exception 都不能离开 DLL。
- 当前 `result()`、`error()` 返回 `std::string` 副本，分配失败可能抛出；对应 size/copy 导出必须 catch-all。
- `shutdown_all()` 内部 vector 分配也可能抛出，必须转换为稳定错误或使用不抛异常的关闭路径。
- status/stop/release 必须对无效、重复和已释放 handle 返回稳定状态。
- DLL unload 前必须确认所有 capture 已 release/join，wrapper 不能持有悬空函数指针。
- API v1 保持向后兼容；需要破坏性变更时新增 ABI version，不修改既有导出语义。

性能方面，size/copy 加一次 JSON 内存复制，相对 Tracy capture、trace 写盘和 JSON 解析成本不是主要瓶颈。只有真实 profile 证明复制成为瓶颈后，才考虑 caller-owned streaming/chunk API；不能为省一次复制而暴露 `std::string`、`std::vector` 或 Tracy 对象。

### 13.4 源码归属

唯一权威源码已落在 `components/profiler/tracy-bridge`。VS Code 插件和 MCDK 发布流程应消费同一个构建产物；MCDevTool 不依赖本机相邻目录 `D:\Zero123\CPP\mcdev-tools`。

### 13.5 Native endpoint 发现契约

后端发现算法必须与插件侧保持同一业务规则，但不照搬插件页面激活后的周期扫描生命周期。

固定规则：

- 只检查 IPv4 TCP LISTEN socket。
- 只接受端口闭区间 `8086..8105`。
- socket owning PID 必须严格等于当前 MCDK 启动并持有的 Minecraft PID。
- 当前 Minecraft 进程必须仍然存活，且进程身份/generation 与创建当前 runtime context 时一致，不能只信任一个可能被系统复用的 PID 数字。
- 连接地址固定为本机回环地址；Agent 不能提供 host、port 或 PID 覆盖自动发现结果。
- 候选按 port 排序以保证诊断输出稳定。
- 没有候选时返回 `TRACY_ENDPOINT_NOT_FOUND`。
- 同一当前游戏 PID 出现多个范围内 LISTEN 候选时返回 `TRACY_ENDPOINT_AMBIGUOUS`，不得猜测端口或按排序静默选择。

Windows 后端使用 IP Helper API `GetExtendedTcpTable` 获取 owning PID，避免依赖 `netstat.exe` 输出文本、本地化和子进程。端口必须按网络字节序正确转换。

防止 TOCTOU 错位访问：

1. Native start 读取当前 `IProcessContext` 的 PID 和进程身份。
2. 枚举范围内 LISTEN socket，并执行 PID 精确匹配。
3. 在调用 `mcdev_tracy_start` 前重新验证游戏进程仍存活、context 未切换，且目标 `pid:port` 仍在 LISTEN 表中。
4. 任一条件变化时返回 `TRACY_ENDPOINT_CHANGED`，不连接旧候选，也不自动尝试其他进程的端口。
5. capture job 持久化实际 PID、port 和进程 identity；游戏退出或 identity 改变时请求停止 capture。

惰性触发规则：

- MCDK 启动、MCP 初始化、stdio bridge 启动、`tools/list`、`/help`、`/guide` 均不得执行 endpoint discovery。
- Python profiler 调用不得执行 endpoint discovery。
- 普通 `/doctor` 只报告平台、组件文件和静态 capability，不枚举端口。
- 只有 `/start kind=native.cpu` 或显式 `/doctor` Native deep check 才执行一次发现。
- `/status`、`/query`、`/detail` 不重新扫描端口。
- 不创建后台 scanner，不设置周期 timer，不缓存候选供下一次 start 直接复用；每次 Native start 都重新发现并复核。

同步策略：

- 将上述规则作为插件和 MCDK 共用的规范，保留相同端口常量、错误分类和候选排序。
- 两侧使用同一组 discovery golden cases，覆盖范围边界、非 LISTEN、其他 PID、PID 复用、多个候选和本地化输出。
- 插件仍使用文本解析时只共享规范和测试向量；后端不为代码复用而重新引入 `netstat.exe`。
- 插件当前对多个同 PID 候选的隐式选择行为需要改为歧义失败，避免两侧继续保留不同的安全语义。

## 14. CMake 第三方依赖和缓存

### 14.1 目标关系

接入前置条件：根项目最低 CMake 版本从 3.15 统一提升到 3.24，并在顶层第一次 `project()` 前启用 `CMP0091 NEW`。不能让 3.15 顶层直接进入要求 3.24 的 bridge 子目录。

```cmake
cmake_minimum_required(VERSION 3.24)
cmake_policy(SET CMP0091 NEW)
project(MC_DEV_TOOL_ROOT LANGUAGES CXX C)
```

Windows x64 target graph：

```cmake
if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_subdirectory(components/profiler/tracy-bridge)
    add_custom_target(mcdk_native_profiler_component DEPENDS mcdev-tracy-bridge)
    add_dependencies(mcdk mcdk_native_profiler_component)
endif()
```

要求：

- 构建 `mcdk` 时可自动构建 DLL。
- `mcdk` 只动态加载 DLL，不链接 bridge C++ implementation。
- build-tree 中将 DLL 复制到约定的 components 目录，便于本地联调。
- CMake `install()` 使用独立 component，例如 `native-profiler`；只有后续实际采用 CPack 时，CPack 才映射同一 component。
- 发行任务可以选择包含或排除该 install component。
- 用户运行时没有安装 DLL，MCDK 仍可启动，只有 Native capability unavailable。
- 非 Windows x64 不创建 bridge target，并在 configure 输出明确状态。
- 不用业务型 Preset 决定是否构建 bridge；支持平台上的 `mcdk` target graph 固定包含该构建依赖。
- bridge 子目录必须在创建 Tracy、Capstone、zstd 和 DLL targets 前，以目录作用域设置 `CMAKE_MSVC_RUNTIME_LIBRARY`；不能依赖 standalone `CMakePresets.json` 中的设置，因为作为子目录构建时该 Preset 不会被读取。
- DLL及其静态依赖必须使用一致的 MSVC runtime。建议保持现有 `MultiThreaded$<$<CONFIG:Debug>:Debug>`，并通过 target property 测试验证，而不是全局 `add_compile_options(/MT)`。
- 为 `mcdk`、`mcdk_stdio_bridge` 和 `mcdev-tracy-bridge` 增加明确 `install(TARGETS ...)`。Native 使用独立 `COMPONENT native-profiler`；core targets 使用 core component。
- 当前发布工作流是手工复制产物，第一步先落地 CMake install component；只有项目实际采用 CPack 时再增加 CPack 配置，计划不预设 CPack 已经存在。

### 14.2 FetchContent 固定依赖

继续使用 CMake `FetchContent`，固定：

- Tracy commit URL 和 `URL_HASH SHA256`。
- Capstone commit URL 和 `URL_HASH SHA256`。
- `DOWNLOAD_EXTRACT_TIMESTAMP FALSE`。

禁止跟随 branch、latest tag 或无 hash URL。

### 14.3 缓存分层

不能把多个顶层构建共享到同一个可写 CMake binary tree。缓存分为下载缓存、可选只读源码缓存和编译器对象缓存：

```text
dependency-cache/
  downloads/                 # 可跨构建复用的原始归档
  sources/<name>-<hash>/     # 可选、预填充后只读的解压源码

<each-top-level-build>/_deps/ # 每个顶层 build 独占，不共享
```

新增 CMake cache variables：

```cmake
MCDEV_DEPS_DOWNLOAD_CACHE    # 共享下载归档目录
MCDEV_DEPS_SOURCE_CACHE      # 可选、由外部预填充的只读源码目录
```

规则：

- `FetchContent_Declare(... DOWNLOAD_DIR ... DOWNLOAD_NAME ...)` 指向下载缓存，归档存在且 hash 正确时复用。
- 归档文件名包含项目名和固定 revision，避免同名 URL 冲突。
- 普通 configure 只共享归档，不自动向全局 source cache 并发解压。
- 可选 source cache 必须由独立预填充步骤生成，以内容 hash/version 命名，完成后只读；通过 `FETCHCONTENT_SOURCE_DIR_TRACY`、`FETCHCONTENT_SOURCE_DIR_CAPSTONE` 显式使用。
- 每个顶层 build 独占自己的 FetchContent source-subbuild/binary directories，禁止共享 `_deps` build 状态。
- `FETCHCONTENT_UPDATES_DISCONNECTED` 可在已固定归档时启用。
- `FETCHCONTENT_FULLY_DISCONNECTED` 只用于确认缓存已预填充的离线构建；缓存缺失时必须明确失败，不能静默使用旧源码。
- CI 缓存 key 包含 Tracy/Capstone URL hash 和 bridge CMake 文件 hash。

### 14.4 编译加速

- 优先使用 `sccache` 缓存第三方对象编译；`/FS` 只解决 MSVC PDB 并发写入，不是编译缓存。
- Tracy、Capstone 编译选项变化必须进入缓存 key。
- 不把生成的 DLL 当作源码缓存；DLL 属于明确 toolchain/release artifact。
- 本地 clean 不默认删除共享 download/source cache。
- 提供显式 `mcdev_clear_dependency_cache` 维护命令，不在普通 configure/build 自动清缓存。

### 14.5 Preset 边界

不为 Native profiler 增加或拆分业务型 Preset。

Preset 只描述构建环境，例如 generator、toolchain、architecture、build type 和必要的环境路径；不得表达以下业务选择：

- 是否构建 Native profiler。
- 是否把 DLL 放入发行包。
- 用户是否安装 Native component。
- 运行时是否启用 Native profiler。

这些职责分别归属：

- 构建关系：CMake target graph。
- 发行内容：CMake install component 和发行任务参数；若后续采用 CPack，再映射为对应 CPack component。
- 用户安装：安装器或离线组件包。
- 运行时使用：组件探测和惰性 DLL loader。

依赖缓存根目录属于构建基础设施，可通过普通 CMake cache variable、环境变量或 `CMakeUserPresets.json` 提供；仓库 Preset 不写开发者机器绝对路径，也不据此派生业务变体。

## 15. Native 构建和发行

构建阶段与发行阶段分离：

```text
cmake build mcdk
  -> 可依赖并构建 mcdev-tracy-bridge.dll
  -> 本地 components 目录可直接测试

cmake install / package
  -> core component 必选
  -> native-profiler component 可选

runtime
  -> 没有 profiler op 时不加载 DLL
  -> 首个 profiler op 探测/校验/尝试加载 DLL，使 help 能反映 capability
  -> 只有 native.cpu start 或 kind=native.cpu, deep=true doctor 扫描 endpoint
```

Release 验证：

- 运行 bridge C ABI tests。
- 生成并校验 DLL SHA-256。
- 安装 Tracy 和 Capstone license。
- 生成 `component.json`。
- 验证 core-only 包没有 DLL 仍能运行 Python profiler。
- 验证 full 包在没有任何 profiler op 时不加载 DLL；首次 `/help` 后 probe 结果进入 runtime capability。

## 16. 实施状态

Phase 0-5 与 Phase 6 的 help/guide 已落地。Debug/Release 离线构建、C ABI 测试、核心 typed API deadline/落盘/恢复测试、stdio 无游戏回退和安装树组件校验已通过。真实游戏中的 Python/Tracy 数据质量、插件侧 discovery golden cases 和 Agent 召回参数校准仍需游戏 E2E；不得把离线测试表述为游戏采集已验证。

### Phase 0：领域 API 与测试骨架

- 建立 `mcdev_profiler_core`，确保不依赖 MCP。
- 建立每个 game runtime 唯一的 `ProfilerRuntimeOwner` 和惰性 `ProfilerServiceProvider`，禁止多实例绕过全局任务锁。
- 定义 typed requests/results/errors、job states、backend interface。
- 建立 fake clock、fake executor、fake store、fake backend。
- 固定 `mc_profiler` adapter schema，但不接真实 profiler。
- 将共享 tool definition 加入 `buildAllTools()`，由内置 MCP 和 `mcdk_stdio_bridge` 同时消费。
- stdio bridge 对 `/help`、`/guide` 先转发，后端不可达时使用共享静态响应回退；其他 op 保持转发。

验收：其他 C++ 调用方可不经过 MCP 完成 fake start/status/query/stop；游戏未启动时 stdio `tools/list` 仍包含 `mc_profiler`，且 `/help` 尝试后端后返回 runtime unknown 的静态回退。

### Phase 1：惰性 service、状态机和落盘

- 实现 service lazy holder、全局任务互斥、soft/completion deadline 和 `cleanup_pending`。
- 实现 data/index/summary/manifest 的顺序提交、原子 rename 和恢复协议。
- 实现 status/history/discard/cleanup domain API 和 MCP 映射。

验收：MCP client start 后消失，fake job 仍自动完成；未调用时无 worker。

### Phase 2：Python CPU

- 迁移 Yappi start、marker、collect、cleanup。
- 使用 512 functions / 2048 edges 的有界快照、稳定 ID、字符串上限和 hard TTL，由 MCDK 持久化后分页查询。
- 支持 client/server/all、CPU/WALL。
- 实现 hotspots/functions/callers/callees/contexts 查询。
- 迁移 parser 和 report golden tests。

### Phase 3：Python memory

- 迁移 tracemalloc baseline/collect/cleanup。
- 增加 duration 和 host watchdog。
- 使用 512 allocation sites、最大 16 层 traceback、路径上限和 hard TTL 的有界快照，由 MCDK 持久化后分页查询。
- 实现 allocations/growth/retained/traceback 查询。
- 保证 IPC 失败走幂等 cleanup。

### Phase 4：CMake Native component

- 建立 bridge 唯一源码位置。
- 接入 FetchContent 分层缓存。
- 建立 mcdk -> bridge target dependency、build-tree copy 和 install component。
- 在发行任务中分别验证包含和排除 Native install component 的包，不新增业务型 Preset。

### Phase 5：Native runtime

- 实现安全 DLL loader 和 component manifest 校验。
- 按 13.5 的惰性发现契约，使用 Windows IP Helper API 在 `8086..8105` 内匹配 LISTEN socket、当前 Minecraft PID 和进程 identity。
- 与插件共用 discovery 规范和 golden cases，并修正多个同 PID 候选时的隐式选择。
- 接入 start/status/stop/result/release 和 watchdog。
- 实现 Native views 和 trace 受控落盘。

### Phase 6：Agent guide 与召回校准

- 实现 help/guide 和上下文 next calls。
- 校准 Top-K、字节上限、duration 和 retention。
- 用真实项目验证 Agent 能通过少量定向 query 定位热点。

## 17. 测试矩阵

### 单元测试

- typed API 与 MCP adapter 的边界。
- op 参数类型、范围、未知字段。
- job 状态转换、并发 start、stop 幂等、discard/finalize 竞争。
- 单一 runtime owner、多 adapter 共用同一 service，以及重复构造被拒绝。
- soft/completion deadline、`cleanup_pending` 和系统时钟跳变隔离。
- Python 有界快照、字符串/记录预算、stable ID、hard TTL 和幂等 cleanup。
- cursor 绑定、sort/filter、条数和字节预算。
- 原子落盘、Unicode 路径、磁盘满、containment。
- Native manifest/hash 一致性、可选签名、ABI、协议和导出函数校验。
- CMake 下载缓存命中、独占 `_deps`、离线缓存缺失诊断和 MSVC runtime 属性。

### 集成测试

- 游戏未进入世界、IPC 断开、collect 中游戏退出。
- MCP client start 后断开且不再调用。
- shutdown 时 Python 或 Native capture 活动。
- DLL 缺失、损坏、不兼容、Tracy endpoint 不存在或 PID 不匹配。
- Native endpoint 范围上下界、其他 PID、PID identity 变化、多候选歧义和连接前二次校验。
- 未调用 Native start 或显式 Native deep doctor 时不执行 `GetExtendedTcpTable`，不存在后台 discovery timer。
- 首个 profiler op 执行一次 DLL probe/load；显式 Native deep doctor 执行一次 discovery、不创建 capture worker，后续普通 `/doctor` 不扫描端口。
- core-only install 和 full install。
- 已填充依赖缓存的离线 rebuild 不访问网络。
- 每个顶层构建独占 `_deps`；跨构建只共享下载归档、可选只读源码缓存和按完整编译环境键控的编译器对象缓存。

### 回归测试

- 迁移插件现有三类 parser 测试向量。
- 迁移 Markdown/SVG report 关键断言。
- 保留 bridge C ABI tests。
- 检查未调用 profiler 时没有新增 worker、端口扫描、报告扫描和 DLL module；首个任意 profiler op 才探测 DLL。
- 游戏未启动时，`mcdk_stdio_bridge tools/list` 包含与内置 MCP 完全相同的 `mc_profiler` JSON schema。
- stdio bridge `/help`、`/guide` 先发起一次有界后端连接尝试，失败后回退到共享静态帮助并标记 runtime unknown；运行态 op 不在本地执行。
- stdio bridge 与旧版 MCDK 不匹配时返回明确版本/工具不可用错误。

## 18. 风险与对策

| 风险 | 对策 |
|---|---|
| MCP 逻辑渗入核心 | 独立 core target，禁止依赖 cpp-mcp，typed API 测试 |
| 内置 MCP 与 stdio bridge 工具定义漂移 | 两者共用 `buildAllTools()` 和 `tryBuildLocalResult()` |
| stdio bridge 与远端 MCDK 版本不匹配 | 成对发行，并返回 `BACKEND_TOOL_UNAVAILABLE` |
| Agent 启动后不停止 | 有限 duration、backend timer、MCDK watchdog、snapshot hard TTL |
| 结果撑爆上下文 | L0-L3、Top-K、cursor、字节预算 |
| 上游采集提前截断导致无法召回 | 显式返回 observed/captured/truncated；Python 重新采集以扩大覆盖，Native 区分完整 trace 与有界 index |
| profiler 互相污染 | 全局单任务互斥 |
| 多个 service 绕过全局互斥 | 每个 game runtime 唯一 owner，所有 adapter 共用 provider |
| Native ABI/协议漂移 | manifest 一致性、ABI、protocol 校验；需要防篡改时验证签名 |
| Native 端口连接到其他游戏或复用 PID | 固定端口范围、PID 与进程 identity 双重匹配、连接前复核、歧义失败 |
| C++ 对象跨 DLL 生命周期不一致 | DLL 边界保持 C ABI，MCDK 侧使用 RAII C++ wrapper |
| C++ exception 穿过 C ABI | 每个导出函数 catch-all，并返回稳定错误状态 |
| FetchContent 每次下载 | 共享 download/source cache、固定 hash、CI cache |
| 共享 CMake build tree 产生竞态或路径污染 | 只共享下载/只读源码缓存，每个顶层 build 独占 `_deps` |
| Native 是否构建、发行、安装、加载职责混淆 | target graph、install component、安装流程、lazy loader 分层负责 |
| DLL 劫持或组件被替换 | 固定目录、绝对路径、安全加载 flags；相邻 SHA-256 只校验一致性，真实性依赖 Authenticode 或内置可信公钥验证的签名 manifest |
| trace 占满磁盘 | job/bytes/TTL/单文件配额 |
| 崩溃遗留状态 | manifest 恢复和有界清理 |

## 19. 完成定义

- MCP `tools/list` 只新增 `mc_profiler`。
- `mcdk_stdio_bridge` 在游戏未启动时也暴露同一 `mc_profiler` schema。
- stdio bridge 对 help/guide 先转发并在不可达时静态回退，所有运行态操作惰性转发且不持有 profiler 任务。
- profiler core 可由 C++ 业务直接调用，不依赖 MCP 或网络回环。
- 每个 game runtime 只有一个 owner；MCP 和其他业务共享同一个惰性 service。
- 未调用时不创建 worker、不扫描端口、不加载 DLL、不扫描历史报告。
- Python profiler 在 Agent 不再调用时仍须按 deadline 自动结束、清理并落盘；Native 须自动请求 stop 和持久化可得结果，若 in-process worker 未在 completion deadline 内退出，则进入 `cleanup_pending`、保持任务锁并等待 cooperative join，不能伪报已清理。
- 默认最多一个 profiler 活动。
- Agent 默认只收到摘要；Python 有界 snapshot 落盘后由 query 分页召回，Native 明确区分完整 trace 和有界分析 index。
- 所有 query 支持服务端筛选、排序、分页和稳定 ID。
- 所有路径受服务端控制，写入原子且有 retention。
- Native DLL 缺失不影响 MCDK 和 Python profiler。
- Native endpoint discovery 与插件共享规则，仅在显式 Native 调用时触发，并拒绝 PID 不符或多候选情况。
- Native DLL 不导出 STL/Tracy C++ 对象，所有导出函数形成完整异常边界。
- MCDK 通过集中式 C++ loader/wrapper 使用 Native C ABI，业务层不直接持有函数指针。
- Native finalizing 超时进入 `cleanup_pending` 并保持任务锁；同进程 DLL 模式不虚假承诺可强杀线程。
- 构建 mcdk 可联动构建 DLL，发行可独立选择 Native install component。
- 固定第三方源码 hash，下载缓存可复用，每个顶层构建独占 FetchContent binary tree。
- 已填充缓存的离线构建可重复完成。
- 所有 partial、timeout、persist failure 返回真实状态，不伪报 completed。

## 20. 游戏 E2E 后需校准的参数

- `duration_seconds` 当前默认 15、范围 1..300；需根据真实开销判断是否按 kind 收紧。
- query/detail 当前最多 50/20 条且约 64 KiB；需用真实符号长度验证估算余量。
- retention 当前为 50 jobs、30 天、2 GiB；Native 单 trace 的更严格配额需用真实 trace 样本确定。
- Markdown/SVG 当前只在 `/export` 生成，`data.json` 当前使用单文件。
- Python 当前采集 512 functions 或 allocations、2048 edges；只有真实召回不足时才引入多页 IPC snapshot 协议。
- Native component 默认进入正式安装包，还是仅进入可选离线组件包。
