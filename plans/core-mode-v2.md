# mcdk_core 核心模式库 v2 计划

> 留档计划。本文记录 feature/core-mode 分支当前进展、遗留问题与 v2 演进方向。
> 创建于核心模式库分离工作的第一阶段（v1 搬迁）完成后。

## 上下文（当前进展）

### 已完成：v1 启动逻辑搬迁（feature/core-mode 分支）

把写死在 `tools/mcdk/main.cpp` 里的启动逻辑抽取为独立的 `mcdk_core` 库，对外暴露纯 API，让其他业务能"几行代码启动"。

**交付物（v1）：**
- `include/mcdk_core/core.h` — 公共伞头
- `include/mcdk_core/launcher.h` — `Launcher`/`LaunchOptions` API 声明
- `src/core.cpp` — 启动逻辑实现（从 main.cpp 逐字搬迁 + 对外 API + 注入式 logger）
- 根 `CMakeLists.txt` 新增 `mcdk_core` target（STATIC，全套链 mcdevtool + mcp + MCDEV_MOD_RESOURCE）
- `tools/mcdk/CMakeLists.txt` 改链 mcdk_core
- `tools/mcdk/main.cpp` 缩为薄壳（1032 → 59 行）
- `tests/core_smoke_test.cpp` — 3 个用例验证纯内存 config + logger 注入
- `docs/core-library.md` — 接入指南
- `modules/console.hpp` 补 functional；`config.hpp` 去 exit；`mod_register.hpp` 加回调参数

**关键决策记录：**
| 决策点 | 选择 | 原因 |
|--------|------|------|
| Core 依赖 | 全套链（mcdevtool + mcp + MCDEV_MOD_RESOURCE） | 业务一行 runCore 即有完整能力 |
| launchGameExe 处理 | 先搬迁不拆 | 保证现有 mcdk.exe 行为 100% 不变 |
| 输出/交互 | 日志全走回调，保留 cin | 平衡解耦与工作量 |

### 关键教训：STATIC vs OBJECT

曾尝试把 mcdk_core 改为 OBJECT（与 mcdevtool 风格一致），**编译失败**。

**根因**：OBJECT 库没有归档，链接器拿到 .obj 后，mcdevtool(OBJECT) → mcdk_core(OBJECT) → mcdk(EXE) 的跨两层 .obj 传播链在链接器层面断开，mcdevtool 的 7 个 .obj 未被列入 link 命令，导致 35 个 LNK2019（全是 MCDevTool::* 符号）。

**结论**：在 mcdevtool OBJECT → mcdk_core → mcdk 这个依赖链结构下，**必须用 STATIC**。STATIC 打包成 .lib 后由链接器自动解出内部 .obj，才是该场景的可靠方案。NBT/mcp/MCDEV_MOD_RESOURCE 不受影响是因为它们都是 STATIC。当前已改回 STATIC，编译通过。

---

## 遗留问题（v2 要解决的）

### 问题一：强制文件写入行为未剥离（高优先级）

原版作为 CLI 工具时合理，但作为库 API 不可接受——业务方用纯内存 config 调 API，结果 API 背着他改磁盘文件、弹 cin 交互。

**强制写入行为分三类：**

#### 第一类：必要的运行时世界产物（无法剥离，不是问题）
这些是 MCPE 引擎的硬性契约，核心模式必须继续写，否则游戏进不去档：
- `level.dat`（世界数据）
- `world_behavior_packs.json` / `world_resource_packs.json`（addon 清单）
- `dev_config.cppconfig`（网易进档参数：玩家名/皮肤/世界 id）
- behavior_packs/resource_packs 目录下的软链接（`cleanRuntimePacks` + `linkSourceAddonToRuntimePacks`）

#### 第二类：配置文件回写（可剥离，是问题）
- `createDefaultConfig()` → 不存在时写回 `.mcdev.json`（config.hpp L75-79）
- `tryUpdateUserGamePath()` → 路径无效时改写 `.mcdev.json` 的 game_executable_path（core.cpp L882）

#### 第三类：交互式输入（可剥离，是问题）
- core.cpp L878-885：游戏路径无效时 `std::cout << "是否更新...?(y/n)"` + `std::getline(std::cin, ...)` 然后回写文件
- `createDefaultConfig()` 里的 `std::cin`（用户手动输入 exe 路径）

**位置清单（core.cpp 内）：**
- L878-885：游戏路径无效的 y/n 交互 + tryUpdateUserGamePath
- L1046-1051：loadConfigFromFile 调 userParseConfig（间接触发 createDefaultConfig 写文件）
- L1053-1054：createDefaultConfig 转发（触发 cin + 写文件）

### 问题二：裸 JSON 入参不利于 API 设计（高优先级）

当前 `LaunchOptions { nlohmann::json config; }` 有三个硬伤：
1. **无 schema 约束**：字段名写错（`"game_path"` vs `"game_exe_path"`）编译期发现不了，运行时静默用默认值——对大量客户是隐蔽 bug。
2. **无文档性**：业务方不知道有哪些字段、哪些必填、类型是什么，要去看 `.mcdev.json` 注释或读源码。
3. **与"强制写文件"耦合**：json 模型本身就是磁盘 `.mcdev.json` 的内存映像，沿用它等于继承"这是个配置文件"的语义。

---

## v2 目标

把核心模式从"CLI 配置文件映像"演进为"真正的库 API"：
1. 强类型 LaunchConfig 结构体取代裸 json
2. 文件写入/交互全部策略化、可控
3. CLI 行为零变化（走兼容适配层）

---

## v2 定稿决策（2026-06-24 代码评审后）

> 下文是把上面草案与真实代码逐行对照后的修正与拍板，后续以本节为准；草案 91-176 行的结构体/步骤为参考示意。

### 决策一：桥接策略 = 方案B（深度重构热路径 helper）

config 不只在 `startGame` 内被读，而是被 **7 个下游 helper 深度消费**（全部吃 `const nlohmann::json&`）：

| Helper | 位置 | 字段块 | v2 处理 |
|--------|------|--------|---------|
| `parseLevelOptionsFromUserConfig`/`createUserLevel` | `modules/level.hpp:13` | world_type/game_mode/world_seed/enable_cheats/keep_inventory/do_weather_cycle/do_daylight_cycle/experiment_options/world_name | **重构为强类型** |
| `getMcpServerConfigFromJson` | `modules/mcp_server.hpp:70` | mcp_server_config{} | **重构为强类型** |
| `UserModDirConfig::parseListFromJson` | `modules/mod_dir_config.hpp:53` | included_mod_dirs[] | **重构为强类型** |
| `launchGameExe` 内 auto_hot_reload_* / skin_info | `src/core.cpp:320` | 热更新开关 / 皮肤 | **重构为强类型** |
| `getPtvsdConfigFromJson` | `modules/env.hpp:166` | ptvsd_debugger{} | 逃生舱（保留 json） |
| `registerDebugMod` 内 debug_options | `modules/mod_register.hpp:50` | debug_options{} | 逃生舱（保留 json） |
| `UserStyleProcessor` | `modules/style_processor.hpp:19` | window_style{} | 逃生舱（保留 json） |

选 B 而非 A（toJson 桥接）：让热路径 helper 真正吃强类型，长期收敛。代价是 step 2「不动 modules」边界作废——**modules helper 的签名会改**（文件位置不动）。

### 决策二：嵌套块建模 = 常用字段强类型 + 高级块 json 逃生舱

- **强类型**：顶层 18 标量 + `experiment_options` + `skin_info` + `included_mod_dirs[]` + `mcp_server_config`。
- **逃生舱（LaunchConfig 上保留 `nlohmann::json` 原样透传）**：`debug_options`、`window_style`、`netease_config`、`modpc_debugger`、`ptvsd_debugger`。这 5 块少改、字段杂，对应 helper 继续吃自己那段 json，不强类型化。

> ⚠️ **决策二已被实现期推翻**（见下方修正六、修正七）：留 5 个 json 逃生舱仍是 json 数驱，不符合核心模式 API 设计。最终只有 `debug_options` 一块保留 json。

### 修正六：逃生舱收敛到只剩 debug_options 一块（推翻决策二的"5 块 json"）

`window_style` / `netease_config` / `modpc_debugger` / `ptvsd_debugger` 都有**固定 schema**，应当强类型化（业务方点字段即配置，不需要知道 json 形状）。最终全部建成强类型子结构：
- `window_style` → `WindowStyle{alwaysOnTop,hideTitleBar,titleBarColor?,fixedSize?,fixedPosition?,lockCorner?}`
- `netease_config` → `NeteaseConfig{chatExtension}`
- `modpc_debugger` → `ModpcDebugger{enabled,port}`
- `ptvsd_debugger` → `PtvsdDebugger{enabled,ip,port}`

**唯一**仍保留 `nlohmann::json` 的是 `debug_options`：它被原样 `dump()` 注入调试 mod 的 `Config.py`（true→True/false→False/null→None），键集由 python mod 自己定义、可独立演进，mcdk 不拥有该 schema——强类型化等于把 C++ API 焊死在 mod 内部选项表上。这是"自由透传"而非偷懒的 json 数驱。

对应 helper：`env.hpp getPtvsdConfigFromJson`→`getPtvsdConfig(PtvsdDebugger)`、`style_processor.hpp` ctor 吃 `WindowStyle`、`netease/modpc` 在 core.cpp 内联读强类型字段；`mod_register.hpp registerDebugMod` 仍吃 `debug_options` 那段 json。

### 修正七：结构体按语义分组（不继承 .mcdev.json 扁平布局）

磁盘 `.mcdev.json` 把 ~30 个字段全堆在根级，纯为向下兼容，很乱。核心模式结构体是新设计，**重新分组**为嵌套子结构：

```
LaunchConfig {
  gameExecutablePath, autoJoinGame, includedModDirs[]   // 顶层
  world  { name, folderName, seed?, reset, type, gameMode, enableCheats,
           keepInventory, doWeatherCycle, doDaylightCycle, experiments? }
  player { name, skin? }
  hotReload { mods, ui, shaders, materials }
  debug  { includeDebugMod, ptvsd{}, modpc{}, options(json) }
  mcpServer{}, netease{}, windowStyle{}                  // 服务/外观
}
```

磁盘格式不受影响：`fromJson`/`toJson` 负责"扁平 json ↔ 嵌套结构体"映射，集中处理所有 key 对应关系。环境变量优先级（`MCDEV_PTVSD_*` / `MCDEV_MODPC_DEBUGGER_PORT` 等）在启动时解析，仍高于配置。

### 修正三：问题一（cin/写文件）真实范围比草案小

core 路径上**只有 `src/core.cpp:874-890` 一处** cin+写盘（路径无效 y/n → tryUpdateUserGamePath）。`createDefaultConfig` 的 cin（`config.hpp:17`）只在 `loadConfigFromFile→userParseConfig` 链上触发——**那是 CLI 路径，core 模式直接构造 LaunchConfig，天然不可达，无需改**。草案"位置清单"里 L1046+/L1053+ 两条因此降级为 CLI-only，非 core 改动点。

### 修正四：API 爆炸半径极小

`LaunchOptions.config`/`Launcher::run` 全仓库仅 4 个调用点：`main.cpp:47-51` + `core_smoke_test.cpp` 三个用例。改字段类型/改签名只波及这两个文件。

### 修正五：真实全字段 checklist（草案 91-123 行结构体不完整 ⚠️）

草案命名与真实 key 对不上（`hotReloadMods`→`auto_hot_reload_mods`、`experiments`→`experiment_options{data_driven_*}`、`mcpServerPort`→`mcp_server_config{server_port}`），且**漏了 5 个嵌套块**（ptvsd_debugger/modpc_debugger/debug_options/window_style/netease_config）——漏掉=core 模式下 ptvsd/mcdbg/按键绑定/窗口样式/网易聊天扩展静默失效。

真实全集（toJson 必须全覆盖；**结构体默认值须逐字段 == helper 的 `.value(key,default)` 默认值**，README 配置表即对照清单）：

```
顶层标量(18): game_executable_path, world_seed, reset_world, world_name,
  world_folder_name, auto_join_game, include_debug_mod, auto_hot_reload_mods,
  auto_hot_reload_ui, auto_hot_reload_shaders, auto_hot_reload_materials,
  world_type, game_mode, enable_cheats, keep_inventory, do_weather_cycle,
  do_daylight_cycle, user_name
嵌套(9): included_mod_dirs[], experiment_options{}, skin_info{},   ← 强类型
  mcp_server_config{},                                            ← 强类型
  modpc_debugger{}, ptvsd_debugger{}, debug_options{},            ← 逃生舱
  window_style{}, netease_config{}                                ← 逃生舱
```

## v2 实施计划

### 设计：两层抽象

#### LaunchConfig（强类型，主路径）
取代裸 json。字段与原 `.mcdev.json` 一一对应，但有类型、有默认值、编译期可检。

```cpp
namespace mcdk::core {
    struct LaunchConfig {
        // === 必填 ===
        std::filesystem::path gameExecutablePath;     // 游戏 exe（必填，无默认）

        // === 世界 ===
        std::string worldFolderName = "MC_DEV_WORLD";
        std::string worldName       = "MC_DEV_WORLD";
        std::optional<int64_t> worldSeed;
        bool resetWorld             = false;
        int  worldType              = 1;   // 0旧版 1无限 2平坦
        int  gameMode               = 1;   // 0生存 1创造 2冒险
        bool enableCheats           = true;
        bool keepInventory          = true;
        bool doWeatherCycle         = true;
        bool doDaylightCycle        = true;
        struct Experiments { bool biomes=false, items=false, molang=false; } experiments;

        // === 启动行为 ===
        bool autoJoinGame        = true;
        bool includeDebugMod     = true;
        std::string userName     = "developer";
        struct SkinInfo { bool slim=false; std::filesystem::path skin; } skinInfo;

        // === 服务开关 ===
        bool hotReloadMods      = true;
        bool hotReloadUi        = false;
        bool hotReloadShaders   = false;
        bool hotReloadMaterials = false;
        bool mcpServerEnabled   = false;
        int  mcpServerPort      = 19133;
        // ... 其余字段按 .mcdev.json 完整映射

        // === 与原 .mcdev.json 的互转（适配层，非主路径）===
        static LaunchConfig fromJson(const nlohmann::json&);
        nlohmann::json toJson() const;
    };
}
```

#### LaunchOptions（策略化，解决问题一）

```cpp
struct LaunchOptions {
    LaunchConfig config;                 // ← 强类型，不再是裸 json
    ConsoleOutputCallback logger = nullptr;
    bool printLogo = false;
    int  pluginEnv = -1;

    // === 文件写入策略（解决第二类问题）===
    enum class ConfigFilePolicy {
        ReadOnly,     // 不读不写 .mcdev.json（纯内存，默认）  ← 核心模式主路径
        ReadOrCreate, // 读，缺失则创建默认（原 createDefaultConfig 行为）
        ReadWrite,    // 读+允许回写（路径修复等，原 CLI 行为）
    };
    ConfigFilePolicy configFilePolicy = ConfigFilePolicy::ReadOnly;

    // === 交互策略（解决第三类问题）===
    // nullptr 时遇到需要交互的场景直接抛异常（核心模式推荐）
    // 非 nullptr 时由业务方提供（如 CLI 注入 std::cin 逻辑）
    using PromptCallback = std::function<bool(const std::string& question)>;
    PromptCallback prompter = nullptr;
};
```

#### 问题解决映射

| 问题 | 解决方式 |
|------|----------|
| 强制写 `.mcdev.json` | `ConfigFilePolicy` 默认 `ReadOnly`，核心模式完全不碰磁盘配置；CLI 传 `ReadWrite` 保持原行为 |
| 强制 cin 交互 | `prompter` 默认 nullptr 时抛异常；CLI 注入 cin-based prompter 保持原行为 |
| 裸 json 无约束 | `LaunchConfig` 强类型结构体，字段错写编译期报错；`fromJson/toJson` 只作兼容适配 |

### 实施步骤

1. **新增 `include/mcdk_core/launch_config.h`**：LaunchConfig 结构体 + fromJson/toJson 适配层。
2. **改造 `startGame`（src/core.cpp）**：内部改读结构体字段（逻辑等价，逐字段对应原 `config.value("xxx", default)`）。startGame 签名从 `(const nlohmann::json&)` 改为 `(const LaunchConfig&, const LaunchOptions&)`。
3. **策略化文件写入/交互**：
   - 游戏路径无效的 y/n 交互（core.cpp L878-885）→ 走 `options.prompter`，nullptr 抛异常
   - `tryUpdateUserGamePath` → 仅 `ConfigFilePolicy::ReadWrite` 时调用
   - `loadConfigFromFile` → 按 policy 决定是否自动创建
4. **CLI main.cpp 走兼容路径**：`LaunchConfig::fromJson(loadConfigFromFile(...))` + `configFilePolicy=ReadWrite` + `prompter=cin版本`，行为零变化。
5. **Launcher::run 签名升级**：接收新的 LaunchOptions（含 LaunchConfig）。
6. **smoke test 升级**：用例改为构造 LaunchConfig 强类型对象。
7. **文档更新**：docs/core-library.md 加 LaunchConfig 字段表 + policy 说明。

### 风险与缓解

| 风险 | 等级 | 缓解 |
|------|------|------|
| startGame 改读结构体时漏字段或默认值不一致 | **高** | fromJson/toJson 与原 `config.value(...)` 逐字段对照表；smoke test 覆盖每个字段 |
| 字段映射遗漏（.mcdev.json 有 ~30 个字段） | 中 | 以 README.md 的 mcdev.json 配置参数表为 checklist 逐一映射 |
| CLI 行为偏差 | 中 | main.cpp 走 ReadWrite + cin prompter，与原行为逐项对照 |
| fromJson/toJson 与磁盘格式不一致 | 中 | 保留 `loadConfigFromFile` + 新增 `LaunchConfig::fromJson`，双层保证 |

### 验收标准

1. `mcdk.exe` 行为与 v1（及原版）完全一致：logo、世界生成、addon、IPC/MCP/热更新、子进程模式、调试器附加。
2. 核心模式 `LaunchConfig` 强类型对象启动，**不读不写 `.mcdev.json`**，**无任何 cin 交互**。
3. 业务方填错字段编译期报错（强类型保证）。
4. smoke test 全部通过。

---

## v1 当前状态快照（feature/core-mode 分支）

- 编译：✅ 通过（STATIC 库方案）
- mcdk.exe 行为：✅ 与原版一致（待用户实测确认）
- core_smoke_test：✅ 3/3 通过（验证纯内存 config + logger 注入）
- 已知遗留：⚠️ 问题一（强制写文件）、问题二（裸 json）—— 即 v2 目标

## 不做的事（边界）

- 不拆 `launchGameExe` 为 prepareWorld/runProcess（留作 v3，本期保持单入口零行为变更）
- 不动 `cli.cpp`、不动 modules 文件位置、不动 xmake.lua（非主推）
- 不改 mcdevtool 核心 API 层（env/level/addon/utils/debug/style/reload 的 .h/.cpp）
- ⚠️ 决策一（方案B）已推翻"不动 modules 签名"：热路径 helper（level.hpp / mcp_server.hpp / mod_dir_config.hpp）的**签名会改为吃强类型**，但文件位置与 mcdevtool 核心 API 层仍不动；逃生舱的 3 个 helper（env ptvsd / mod_register debug_options / style_processor）签名保持不变。

## 文件索引

- v1 实现：`src/core.cpp`、`include/mcdk_core/{core.h,launcher.h}`
- v1 文档：`docs/core-library.md`
- 配置参考：`README.md`（mcdev.json 配置参数表，作为 v2 字段映射 checklist）
- 构建：根 `CMakeLists.txt`（mcdk_core STATIC target）、`tools/mcdk/CMakeLists.txt`
