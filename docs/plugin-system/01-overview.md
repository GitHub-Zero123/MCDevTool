# 01 · 背景、目标与架构

上级索引：[README.md](README.md)

## 1. 背景

MCDevTool 迭代至今，核心能力（世界生成、Addon 链接、热更新、MCP、Host Bridge、Profiler）已经稳定，但功能边界正在失控：每个开发者期望的工作流不同，需求以 fork + PR 的形式汇入主干，导致

- 主干承载了大量只对少数人有意义的定制逻辑；
- `launchGameExe` 等入口函数持续膨胀，新功能只能继续往里塞；
- 定制需求的作者必须理解整个仓库才能改动一处；
- 上游一旦重构，所有 fork 同时失效。

结论是需要一个稳定的扩展边界，让定制能力以独立二进制的形式存在于主干之外。

## 2. 目标

1. **ABI 兼容性是第一目标。** 插件以动态库形式分发，宿主与插件可以由**不同编译器、不同标准库、不同 CRT、不同优化选项**构建，且必须能在同一进程内共存。这一条优先于接口的美观程度和实现便利性。
2. 插件作者写现代 C++，不接触任何 C ABI 细节。
3. 提供覆盖现有能力的通用 API：控制台、配置、路径、游戏控制、MCP 工具注册、Host Bridge RPC 注册、热更新规则、Pack 干预、任务调度、私有存储。详见 [05-interfaces.md](05-interfaces.md)。
4. 提供事件系统，线程语义明确。详见 [04-events.md](04-events.md)。
5. 用户侧接入成本低：`add_subdirectory` 或 `FetchContent` 之后一条 `mcdk_add_plugin()` 即可出产物。详见 [07-sdk.md](07-sdk.md)。
6. ABI 的向前/向后兼容性由 CI 持续验证，而非靠文档约定。详见 [09-compatibility.md](09-compatibility.md)。

## 3. 非目标

v1 明确不做：

- **热卸载 / 热重载插件。** 改动插件需重启 mcdk。Godot 4.2 支持该能力，但要求每个对象实例可被重建（`recreate_instance_func`），复杂度与收益不成比例。
- **插件沙箱与权限强制。** v1 的 `permissions` 字段只用于展示和审计，不做运行期拦截。
- **进程外插件。** 接口形状会为此预留（见 [10-roadmap.md](10-roadmap.md) §3），但 v1 只做进程内。
- **mcdk 自带脚本语言支持。** v1 不内置任何解释器。

  但**第三方可以自己做**：一个插件 DLL 完全可以充当别的语言的加载器，靠 `config` 区分实例。这条路 v1 就是通的，见 [07-sdk.md](07-sdk.md) §5 与 `examples/02-loader/`。多实例与动态身份两项设计正是为它准备的。

## 4. 总体架构

### 4.1 三层

```text
┌──────────────────────────────────────────────────┐
│ 插件作者代码（现代 C++，异常、STL、继承随意用）        │
├──────────────────────────────────────────────────┤
│ mcdk-plugin-sdk（C++，随插件一起从源码编译）          │ ← 消化插件侧所有 C++ 特性
├════════════════ 纯 C ABI 边界 ════════════════════┤ ← 只有定宽标量、指针、函数指针
│ plugin-host（C++，宿主侧 shim + 实现）              │ ← 消化宿主侧所有 C++ 特性
├──────────────────────────────────────────────────┤
│ mcdk_runtime / mcdk_core（现有实现）                │
└──────────────────────────────────────────────────┘
```

SDK **禁止**以预编译二进制分发，必须随插件工程一起从源码构建。这是整套设计成立的前提：SDK 与插件作者使用同一套编译器、同一份标准库、同一个 CRT，因此 SDK 与插件之间可以自由使用 C++；SDK 与宿主之间才是真正的 ABI 边界。godot-cpp 采取的是同一策略。

### 4.2 ABI 单一真源

ABI 头文件**禁止**在宿主和 SDK 中各存一份。物理上只有一份文件，通过 CMake `INTERFACE` 目标 `mcdk::plugin-abi` 同时被 `plugin-host` 和 `mcdk-plugin-sdk` 消费。任何一侧改动 ABI 头，另一侧立即参与编译验证。

## 5. 仓库目录划分

```text
MCDevTool/
├─ sdk/plugin-sdk/                     # 用户侧，可单独 clone / FetchContent / add_subdirectory
│  ├─ CMakeLists.txt                   # mcdk::plugin-abi (INTERFACE) + mcdk::plugin-sdk (STATIC)
│  ├─ cmake/
│  │  ├─ McdkAddPlugin.cmake
│  │  └─ mcdk-plugin-sdk-config.cmake.in
│  ├─ include/mcdk/plugin/
│  │  ├─ abi/                          # 纯 C，唯一真源，宿主也消费这一份
│  │  │  ├─ core.h                     # 基础类型 / 状态码 / 版本宏 / MCDK_CALL
│  │  │  ├─ entry.h                    # 入口符号 + host_info + plugin_desc
│  │  │  ├─ events.h                   # 事件 id 与 payload 结构体
│  │  │  └─ iface/
│  │  │     ├─ core.h  mem.h  console.h  config.h  paths.h
│  │  │     └─ game.h  mcp.h  rpc.h  hotreload.h  pack.h  task.h  store.h
│  │  ├─ plugin.hpp                    # 伞头文件，用户只 include 这个
│  │  ├─ context.hpp  events.hpp  console.hpp  config.hpp  game.hpp  mcp.hpp  ...
│  │  └─ detail/
│  │     ├─ barrier.hpp                # 异常屏障
│  │     ├─ iface_cache.hpp            # 接口表缓存 + struct_size 能力探测
│  │     └─ trampoline.hpp             # 虚函数 → C 函数指针表
│  ├─ src/bootstrap.cpp                # 唯一的 .cpp：入口胶水 + 错误槽
│  ├─ examples/
│  │  ├─ 00-abi-conformance/           # ABI 一致性测试插件，见 09-compatibility.md
│  │  ├─ 01-hello/
│  │  ├─ 02-mcp-tool/
│  │  └─ 03-hotreload/
│  ├─ templates/plugin-template/       # 供用户复制的起步工程
│  └─ docs/
├─ tools/mcdk/{include/mcdk,src}/plugin_host/   # 宿主侧实现（落地时从 components/ 移到这里：
│  ├─ include/mcdk/plugin_host/
│  │  ├─ manifest.hpp   loader.hpp   registry.hpp
│  │  ├─ event_bus.hpp  host.hpp      guard.hpp
│  └─ src/
│     ├─ manifest.cpp                  # plugin.json 解析与校验
│     ├─ loader.cpp                    # 动态库加载 / 符号解析 / 版本校验 / 卸载
│     ├─ event_bus.cpp                 # 队列 / 线程 / 优先级 / 超时监测
│     ├─ guard.cpp                     # host::guard 异常屏障 + 错误槽
│     └─ interfaces/                   # 一个接口一个文件
│        ├─ core.cpp  mem.cpp  console.cpp  config.cpp  paths.cpp
│        └─ game.cpp  mcp.cpp  rpc.cpp  hotreload.cpp  pack.cpp  task.cpp  store.cpp
├─ tests/plugin_abi/                   # ABI 兼容性矩阵测试
│  └─ golden/                          # 历次 ABI 版本的预编译插件二进制
└─ tools/mcdk/                         # 安装 host、推进阶段、发射事件
```
