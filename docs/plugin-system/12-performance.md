# 12 · 性能契约与热路径规则

上级索引：[README.md](README.md)

## 1. 零插件开销契约（规范）

> **未加载任何插件时，插件系统在每个事件发射点上的开销必须不超过：一次 relaxed 原子读 + 一次可预测分支。除此之外不得有任何额外工作。**

这是硬指标，不是努力方向。绝大多数 mcdk 用户不会装插件，他们**必须**感觉不到插件系统的存在。

由此派生四条禁令，任一违反即视为缺陷：

1. **禁止在分支外构造 payload。** 时间戳、字符串转换、路径转 UTF-8、JSON 序列化——全部只能在确认有订阅者之后做。
2. **禁止在发射点加锁。** 订阅计数只能用无锁原子读。
3. **禁止在发射点做哈希查找。** 事件 id 是编译期常量，直接索引数组。
4. **禁止为事件预先分配。** 队列、缓冲区一律惰性创建。

第 1 条是最容易违反的：`emit(EventId::LogLine, buildPayload(line))` 这种写法在语法上很自然，但它在零插件时仍要付全部构造成本。

### 1.1 契约适用于发射点以外（规范）

> **零插件时，插件系统不得引入任何周期性唤醒、后台线程或定时器。**

发射点的开销容易盯，容易漏掉的是「为了让插件有机会被调度而加的轮询」。典型错误是把主线程的
`WaitForSingleObject(INFINITE)` 改成定时轮询以便抽干 `post_main` 队列——哪怕只有 20 次/秒，那也是
零插件用户白白多付的错。

**正确做法是事件驱动而非轮询。** 主线程唤醒信号（`enableMainThreadSignal()`）只在真的加载了至少一个
插件时创建；没有它时 `mainThreadWorkWaitHandle()` 返回 `nullptr`，等待循环退回无期限阻塞。这样
即使是「一个 if」都省了，而且有插件时的延迟反而比轮询更低。该契约由 `plugin-host` 测试看守。

## 2. 发射点的标准写法（规范）

所有发射点**必须**使用统一宏，**禁止**手写 `if` 加 `dispatch`——手写会逐渐分化，几年后没人知道哪些发射点是安全的。

```cpp
// tools/mcdk/include/mcdk/plugin_host/emit.hpp
namespace mcdk::plugin_host {

    inline constexpr std::size_t kEventCount = static_cast<std::size_t>(EventId::Count);

    // 唯一的热路径状态：约 100 字节，常驻 L1
    extern std::array<std::atomic<std::uint32_t>, kEventCount> gSubscriberCount;

    [[nodiscard]] inline bool hasSubscribers(EventId id) noexcept {
        return gSubscriberCount[static_cast<std::size_t>(id)]
                   .load(std::memory_order_relaxed) != 0;
    }

    template <class Factory>
    void dispatch(EventId id, Factory&& makePayload);   // 仅在确有订阅者时调用

} // namespace mcdk::plugin_host

#define MCDK_EMIT(eventId, makePayload)                                     \
    do {                                                                    \
        if (::mcdk::plugin_host::hasSubscribers(eventId)) [[unlikely]] {    \
            ::mcdk::plugin_host::dispatch((eventId), (makePayload));        \
        }                                                                   \
    } while (0)
```

调用侧——注意 `nowMs()` 与字符串转换都在工厂 lambda 内，零插件时一次都不执行：

```cpp
MCDK_EMIT(EventId::LogLine, [&] {
    return mcdk_ev_log_line{
        .struct_size  = sizeof(mcdk_ev_log_line),
        .channel      = MCDK_LOG_CHANNEL_STDOUT,
        .timestamp_ms = nowMs(),
        .text         = toAbiStr(line),
    };
});
```

零插件时这段展开后等价于 `if (counter.load(relaxed)) { }`：一次 L1 命中的加载、一次几乎永远不跳转的分支。工厂 lambda 按引用捕获、不逃逸，优化后不产生任何对象。

`EventId` 是一个扁平的强类型枚举，与 [13-registry.md](13-registry.md) 的事件登记表一一对应，`EventId::Count` 结尾。ABI 侧的数值 id 由 `resolve()` 在运行期从名字映射过来，两者分离——外部名字可以稳定，内部索引可以随意重排。

## 3. 事件总线惰性化（规范）

| 条件 | 必须成立的状态 |
| --- | --- |
| `.mcdev.json` 无 `plugins` 或全部 `enable: false` | 不加载任何动态库、不创建任何线程、不分配任何队列 |
| 已加载插件，但无 `MCDK_DISPATCH_QUEUED` 订阅 | 不创建派发线程 |
| 出现第一个 QUEUED 订阅 | 此时才创建派发线程与有界队列 |

`PluginHost::advance(stage)` 在插件列表为空时**必须**立即返回，不进入任何循环。

## 4. 编译期总开关

提供 `MCDK_ENABLE_PLUGINS`（默认 `ON`）。关闭时 `MCDK_EMIT` 展开为空语句，`plugin-host` 不参与链接。这既服务于最小化构建，也是第 6 节基准测试的对照组。

**规范：`MCDK_EMIT` 的参数在开关关闭时禁止被求值，但必须仍然通过语法与类型检查**，否则关掉开关构建一次就会暴露一堆只在某一配置下才编译的死代码。用 `if constexpr (false)` 之类的手法保留检查。

## 5. 维护性约束

性能优化最常见的代价是可维护性，这里靠三条约束抵消：

1. **发射点只有一种写法**（§2 的宏），新增发射点无需理解背后的机制；
2. **热路径状态只有一处**（`gSubscriberCount` 数组），订阅与退订各自维护它，没有第二份缓存需要同步；
3. **计数器的增减集中在 `subscribe`/`unsubscribe` 两个函数内**，禁止在别处直接改。插件终结时按 [03-abi-reference.md](03-abi-reference.md) §5 注销订阅，计数自然归零。

## 6. 性能回归防护（规范）

CI **必须**包含一项日志吞吐基准，三组对照：

| 组 | 配置 |
| --- | --- |
| A | `MCDK_ENABLE_PLUGINS=OFF` |
| B | 开启插件系统，加载 0 个插件 |
| C | 开启插件系统，加载 1 个订阅 `mcdk.log.line` 的空插件 |

**判据：B 相对 A 的差异必须落在测量噪声内。** 这是零开销契约的唯一客观检验方式——没有这个基准，契约就只是一句愿望，几次重构之后就会悄悄失效。

C 组不设阈值，只记录并跟踪趋势，用于回答"装一个插件到底要付多少钱"。

日志路径是唯一需要基准的热路径：其余事件的发射频率都在每次启动个位数量级。

### 6.1 实现：`tests/plugin_event_bench.cpp`

基准**没有**做成「构建两次再比对」——那需要 CI 编排，本地跑不了，也就没人会跑，最终会变成只在合并时
才发现的问题。改成在**同一个进程**里量同一条循环的两个版本：一条只做基线工作，一条在基线之外
加上发射点。两者之差就是发射点的真实成本；而 A 组的成本按定义是 0，所以「B 减基线」就是「B 减 A」。
它因此是一个普通的 ctest 用例（`plugin-event-bench`），改坏了当场就能看到。

阈值定在 **15 ns/发射**。真实值在 2ns 量级，阈值放宽是为了容忍 CI 机器的噪声与降频，同时仍能挡住
「把 payload 构造搬到了分支外面」这类回归——那类回归会把单次成本推到几十上百纳秒。

当前实测（未开优化的本地构建，因此是保守值）：

| 组 | 每次发射成本 |
| --- | --- |
| A（开关关闭） | -0.05 ns（即噪声本身；发射点已在预处理阶段消失） |
| B（0 订阅者） | **约 2.3 ns** |
| C（1 个 QUEUED 订阅者） | 约 530 ns（含深拷贝、入队、唤醒派发线程） |

C 组那个数字值得说一句：它的大头不在拷贝，而在发射线程与派发线程争用同一把锁。按日志每秒数百行
算，530ns × 500 行 = 0.03% 单核，完全可接受；但如果将来出现真正的高频事件，这把锁就是第一个要拆的东西。

### 6.2 `MCDK_ENABLE_PLUGINS`

CMake 选项，默认 `ON`。关闭时：

- `MCDK_EMIT` / `MCDK_EMIT_VETOABLE` 在预处理阶段就展开成空，连订阅计数的原子读也不剩；
- `Host::loadDeclared` 拒绝加载并告警一次。**不能只去发射点而照常加载**：那会得到一个插件跑着、
  但一个事件也收不到的半残状态，比干脆不加载难排查得多；
- 插件相关的 ctest 用例自动不注册。
