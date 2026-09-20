# 09 · 兼容性验证

上级索引：[README.md](README.md)　前置阅读：[02-abi-contract.md](02-abi-contract.md)

**规范：ABI 兼容性不靠文档约定保证，靠 CI 持续验证。** 本章是 [01-overview.md](01-overview.md) §2 目标 6 的落实。

## 1. 编译器矩阵

CI 用 [02-abi-contract.md](02-abi-contract.md) §1 表中的每一种工具链构建 `examples/00-abi-conformance`，产出多个 DLL，然后**由同一个宿主测试二进制同时加载全部产物**并运行一致性套件。任一组合失败即视为 ABI 回归。

"同时加载"是关键：分别加载只能证明各自能跑，同时加载才能暴露 CRT 冲突、符号插入、分配器错配这类真实问题。

## 2. 一致性套件内容

`examples/00-abi-conformance` 插件必须覆盖：

- **异常屏障（插件 → 宿主）**：每一个回调点都主动抛出 `std::exception` 派生类、自定义类型、以及非 `std::exception` 派生对象（如 `throw 42`），验证屏障生效且宿主行为符合 [02-abi-contract.md](02-abi-contract.md) §4.2 的表格；
- **异常屏障（宿主 → 插件）**：主动触发宿主侧会抛异常的路径（如传入不存在的皮肤路径、非法 JSON），验证插件侧收到的是状态码而非异常；
- **可否决事件的异常语义**：在 `.before` 事件处理器中抛异常，断言等效为 `CONTINUE` 而非 `VETO`——这是最容易写错、后果最严重的一条；
- 每一个接口函数的正常路径与错误路径；
- 三种内存所有权形态（借用 / 调用方缓冲 / 宿主分配器）各自的分配释放闭环；
- 跨线程订阅与三种派发模式的线程归属断言；
- 静态初始化期不触碰宿主的断言；
- `MCDK_ERR_WRONG_STAGE`：在错误阶段调用注册类 API。

该插件还需提供 `-fno-exceptions` 构建变体，验证屏障退化后 ABI 签名不变。

## 3. 双向版本矩阵

只测"当前 SDK + 当前宿主"没有意义。**必须**同时测：

1. **向后兼容**：`tests/plugin_abi/golden/` 中**历次 ABI 版本预编译的插件二进制** + 当前宿主。这些二进制体积很小，随版本递增入库，**永不重编**——这是唯一能真正证明老插件仍可用的方法。
2. **向前兼容**：当前 SDK 构建的插件 + 旧宿主，验证 `struct_size` 探测正确降级、缺失能力返回 `MCDK_ERR_NOT_SUPPORTED` 而非崩溃。

golden 目录的入库规则：每次 `MCDK_ABI_VERSION_MINOR` 递增时，用该版本的 SDK 以 MSVC `/MD` 与 MinGW 各构建一份 `00-abi-conformance` 存入，附带构建时的工具链信息。

## 4. 构建期闸门

**规范：以下两个目标属于默认构建，不是 ctest 用例。** ABI 被破坏时应当在构建期就失败，而不是等到跑测试。实现位于 `sdk/plugin-sdk/checks/`，由 `MCDK_PLUGIN_SDK_BUILD_CHECKS` 开启（构建测试开启时自动打开）。

| 目标 | 拦的是什么 |
| --- | --- |
| `mcdk_abi_c99_check` | 用 **C 编译器**单独编译全部 ABI 头。任何 C++ 构造——最常见的是 `std::string`、`std::function`——立刻失败 |
| `mcdk_abi_layout_check` | 包含 `abi/abi_assert.h`，对每个跨界结构体断言标准布局、**平凡可复制**、平凡可析构、`struct_size` 在偏移 0、以及关键字段的 `offsetof` |

两者分工明确，缺一不可。已实测三种破坏方式：

| 注入的破坏 | C99 闸门 | 布局闸门 |
| --- | :-: | :-: |
| 调换 `mcdk_str` 的字段顺序 | 通过 | **拦下**（`ptr must come first`） |
| 直接加 `std::string host_version` | **拦下**（`expected C++ compiler`） | — |
| 用 `#ifdef __cplusplus` 藏起 `std::string` | 通过 | **拦下**（`must be trivially copyable`） |

第三行是最要命的情形：C 与 C++ 两侧看到的结构体大小不同，跨 DLL 传递时不报错、只静默损坏内存。单靠 C99 编译检查拦不住它，`is_trivially_copyable` 才是那道闸门。**新增任何 ABI 结构体时，必须同时在 `abi_assert.h` 中补上对应的 `MCDK_ABI_CHECK_GROWABLE`**，否则它不受任何保护。

## 5. CI 静态检查

脚本对 ABI 头与宿主 shim 做机械检查，任一项失败即阻断合并：

| 检查 | 目标 |
| --- | --- |
| ABI 头中出现 `long` / `unsigned long` / `wchar_t` / C++ `bool` / `typedef enum` / 位域 / `#pragma pack` | [02](02-abi-contract.md) §5.2–5.6 |
| ABI 头中出现复合类型的按值参数或返回值 | [02](02-abi-contract.md) §5.7 |
| ABI 头能被 C99 编译器单独编译通过 | [02](02-abi-contract.md) §5.11 |
| 可增长结构体首字段不是 `uint32_t struct_size` | [02](02-abi-contract.md) §5.8 |
| 函数指针缺少 `MCDK_CALL` | [02](02-abi-contract.md) §5.1 |
| `plugin_host/interfaces/` 下存在未经 `host::guard` 的导出函数 | [02](02-abi-contract.md) §4.3 |
| ABI 函数字段缺少 `@name` / `@since` 注释 | [02](02-abi-contract.md) §8 |
| 与上一版 ABI 头比对，出现字段删除 / 类型变更 / 顺序调整 / 枚举值复用 | [02](02-abi-contract.md) §5.9–5.10 |
| 登记表与 ABI 头的 `@name` / `@since` 双向不一致 | [13](13-registry.md) §1.1 |
| 登记表中状态为"可用"的项未被一致性套件覆盖 | [13](13-registry.md) §1.2 |
| 事件发射点未使用 `MCDK_EMIT` 宏 | [12](12-performance.md) §2 |

倒数第四项是 ABI 演进的真正守门人：它把"只增不改"从人的纪律变成机器的判定。登记表的双向校验同理——只查一个方向的话，删头里的注释或删表里的行都能蒙混过关。

## 6. 性能回归

零插件开销契约的基准测试同属 CI 必须项，定义见 [12-performance.md](12-performance.md) §6。

## 7. 运行期自检

宿主在加载插件后**应该**做一次轻量自检并记入日志：插件报告的 ABI 版本、各接口表的 `struct_size`、插件实际取用了哪些接口。该信息在用户报告"插件在我这不工作"时是第一手诊断依据。
