#include <mcdk/plugin_host/host.hpp>

#include <mcdk/console_output.hpp>
#include <mcdk/version.hpp>
#include <mcdk/plugin_host/events.hpp>
#include <mcdk/plugin_host/guard.hpp>

#include "images.hpp"
#include "manifest.hpp"
#include <mcdk/runtime/mcp_tool_registry.hpp>

#include <mcdk/plugin_declarations.hpp>

#include "registry.hpp"

#include <algorithm>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include <mcdk/plugin/abi/entry.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace mcdk::plugin_host {

    namespace {

        // 宿主版本与 mcdk 本体同一个值，真源在 mcdk/version.hpp。
        constexpr std::string_view kHostVersion = mcdk::kVersion;

        void report(const std::string& message, ConsoleColor color) {
            const auto& output = detail::outputCallback();
            if (output) {
                output("[Plugin] " + message, color);
            }
        }

        [[nodiscard]] mcdk_str toAbi(std::string_view text) noexcept {
            mcdk_str out;
            out.ptr = text.data();
            out.len = text.size();
            return out;
        }

        [[nodiscard]] std::string toString(mcdk_str text) {
            if (text.ptr == nullptr || text.len == 0) {
                return {};
            }
            return std::string(text.ptr, text.len);
        }

        [[nodiscard]] void* loadModule(const std::filesystem::path& path, std::string& error) {
#ifdef _WIN32
            // LOAD_WITH_ALTERED_SEARCH_PATH 让插件自带的依赖 DLL 能在它自己的
            // 目录里被找到，要求传绝对路径。
            HMODULE module = ::LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (module == nullptr) {
                error = "LoadLibraryExW failed, GetLastError=" + std::to_string(::GetLastError());
                return nullptr;
            }
            return static_cast<void*>(module);
#else
            // RTLD_LOCAL：避免插件与宿主各自静态链接的同名第三方库发生符号插入
            // （02-abi-contract.md §5.12）。
            void* module = ::dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
            if (module == nullptr) {
                const char* message = ::dlerror();
                error               = message != nullptr ? message : "dlopen failed";
                return nullptr;
            }
            return module;
#endif
        }

        [[nodiscard]] void* findSymbol(void* module, const char* name) noexcept {
#ifdef _WIN32
            return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(module), name));
#else
            return ::dlsym(module, name);
#endif
        }

        const void* MCDK_CALL getInterfaceThunk(const char* name, std::uint32_t version) noexcept {
            const void* table = nullptr;
            guardVoid([&] { table = detail::findInterfaceTable(name, version); });
            return table;
        }

        [[nodiscard]] const char* stageName(mcdk_stage stage) noexcept {
            switch (stage) {
            case MCDK_STAGE_REGISTER:
                return "register";
            case MCDK_STAGE_CONFIG:
                return "config";
            case MCDK_STAGE_WORLD:
                return "world";
            case MCDK_STAGE_RUNTIME:
                return "runtime";
            case MCDK_STAGE_SHUTDOWN:
                return "shutdown";
            default:
                return "unknown";
            }
        }

    } // namespace

    class Host::Impl {
    public:
        explicit Impl(ConsoleOutputCallback output) { detail::setOutputCallback(std::move(output)); }

        void
        loadDeclared(const std::vector<PluginDeclaration>& declarations, const std::filesystem::path& baseDirectory) {
            if (declarations.empty()) {
                return;
            }
#if !MCDK_ENABLE_PLUGINS
            // 开关关闭时不能只把发射点去掉而照常加载：那会得到一个插件跑着但
            // 一个事件也收不到的半残状态，比干脆不加载难排查得多。
            report(
                "插件系统在本次构建中已关闭（MCDK_ENABLE_PLUGINS=OFF），"
                    + std::to_string(declarations.size()) + " 条声明全部跳过",
                ConsoleColor::Yellow
            );
            return;
#else
            // 第 1~4 步：筛出已启用的条目，逐条解析清单。这一轮**不加载任何代码**：
            // 依赖排序必须在第一个 LoadLibrary 之前完成，否则排序就没意义了。
            std::vector<Candidate> candidates;
            std::size_t            disabled = 0;
            for (const auto& declaration : declarations) {
                if (!declaration.enabled) {
                    ++disabled;
                    continue;
                }
                if (auto candidate = prepare(declaration, baseDirectory)) {
                    candidates.push_back(std::move(*candidate));
                }
            }

            // 第 5 步：拓扑排序。
            const auto ordered = topologicalOrder(candidates);

            // 第 6 步：按序加载。
            std::size_t loaded = 0;
            for (const auto* candidate : ordered) {
                if (loadOne(*candidate)) {
                    ++loaded;
                }
            }
            report(
                "已加载 " + std::to_string(loaded) + " 个插件，" + std::to_string(disabled) + " 个已禁用",
                loaded > 0 ? ConsoleColor::Green : ConsoleColor::DarkGray
            );
#endif
        }

        void advance(mcdk_stage stage) {
            if (stage == MCDK_STAGE_SHUTDOWN) {
                mShutdownStageDone = true;
            }
            detail::setCurrentStage(stage);
            // 零插件时这是一次空遍历，没有其他开销。
            detail::registry().forEach([&](mcdk_handle handle, detail::PluginRecord& record) {
                if (record.degraded || record.desc.on_stage == nullptr) {
                    return;
                }
                const mcdk_status status = record.desc.on_stage(record.desc.user, stage);
                if (status == MCDK_OK) {
                    return;
                }
                record.degraded = true;
                if (stage == MCDK_STAGE_REGISTER) {
                    // 注册阶段失败即加载失败：立刻终结，不让它进入后续阶段。
                    report(
                        record.id + " 在 register 阶段失败 (status=" + std::to_string(status) + ")，已卸载",
                        ConsoleColor::Red
                    );
                    terminate(handle, record);
                } else {
                    report(
                        record.id + " 在 " + stageName(stage) + " 阶段失败 (status=" + std::to_string(status)
                            + ")，已降级，后续阶段不再通知它",
                        ConsoleColor::Yellow
                    );
                }
            });
        }

        void shutdown() {
            // 兜底推进 SHUTDOWN 阶段。
            // 正常路径上 launchGameExe 会先 advance(SHUTDOWN) 再调这里，此时这一段
            if (!mShutdownStageDone) {
                advance(MCDK_STAGE_SHUTDOWN);
            }
            detail::registry().forEachReversed([&](mcdk_handle handle, detail::PluginRecord& record) {
                terminate(handle, record);
            });
            // 全部插件终结之后才停派发线程：终结过程本身可能还要抽主线程队列。
            shutdownEventBus();
            // 允许再次加载（测试会这么做）；二次 shutdown 仍是空操作，因为
            // 注册表此时已空。
            mShutdownStageDone = false;
        }

        [[nodiscard]] bool empty() const noexcept { return detail::registry().aliveCount() == 0; }

    private:
        bool mShutdownStageDone = false;

    public:

        // 清单声明的工具录进注册表，handler 留空等插件在注册窗口里 attach。
        void declareMcpTools(runtime::McpToolRegistry& registry) {
            detail::registry().forEach([&registry](mcdk_handle, detail::PluginRecord& record) {
                if (record.degraded) {
                    return;
                }
                for (const auto& tool : record.mcpTools) {
                    if (const auto result = registry.declare(tool, record.id); !result) {
                        report(
                            record.id + " 声明的工具 " + tool.name + " 无法录入："
                                + std::string(runtime::describeMcpToolBindError(result.error())),
                            ConsoleColor::Red
                        );
                    }
                }
            });
        }

        [[nodiscard]] std::vector<LoadedPlugin> loaded() const {
            std::vector<LoadedPlugin> result;
            detail::registry().forEach([&result](mcdk_handle, detail::PluginRecord& record) {
                result.push_back(
                    LoadedPlugin{
                        .id       = record.id,
                        .name     = record.name,
                        .version  = record.version,
                        .path     = record.path,
                        .abiMajor = record.abiMajor,
                        .abiMinor = record.abiMinor,
                        .degraded = record.degraded,
                    }
                );
            });
            return result;
        }

    private:
        // 终结单个插件。顺序是强规范，见 docs/plugin-system/03-abi-reference.md §5.1。
        static void terminate(mcdk_handle handle, detail::PluginRecord& record) {
            // 1~3. 停止派发、等待 in-flight 回调返回、注销全部订阅。
            //      必须在 on_unload 之前完成，顺序反了会把事件打进正在析构的插件对象。
            detachSubscriber(handle);
            // 4. 调用 on_unload
            if (record.desc.on_unload != nullptr) {
                record.desc.on_unload(record.desc.user);
            }
            // 5. 回收其遗留的宿主资源。兑现 05-interfaces.md §6.2：不能因为插件忘了
            //    release 就让内存留到进程结束，但也不能默默收掉——那是插件的 bug，要说出来。
            if (const auto leaked = detail::releaseAllImages(handle); leaked > 0) {
                report(
                    record.id + " 遗留了 " + std::to_string(leaked) + " 个未释放的图像句柄，已代为回收",
                    ConsoleColor::Yellow
                );
            }
            // 6. 作废句柄。此后该插件的任何调用都返回 MCDK_ERR_INVALID_HANDLE 而非崩溃。
            detail::registry().retire(handle);
// 刻意不 FreeLibrary / dlclose：v1 不做热卸载，进程退出时交给操作系统。
// 插件静态对象析构、残留线程、两侧 CRT 卸载顺序叠加，主动卸载的崩溃
            record.module = nullptr;
        }

        // 一条已通过静态校验、等待加载的声明。
        struct Candidate {
            const PluginDeclaration*             declaration = nullptr;
            std::filesystem::path                library;
            // 直指动态库的形态没有清单，此时为 nullopt（也就声明不了依赖）。
            std::optional<detail::PluginManifest> manifest;

            [[nodiscard]] std::string displayName() const {
                return manifest ? manifest->id : library.generic_string();
            }
        };

        // 解析一条声明：定位动态库、读清单、做所有不需要加载代码就能做的校验。
        [[nodiscard]] static std::optional<Candidate>
        prepare(const PluginDeclaration& declaration, const std::filesystem::path& baseDirectory) {
            const auto path  = detail::resolvePluginPath(declaration.path, baseDirectory);
            const auto shown = path.generic_string();

            Candidate candidate;
            candidate.declaration = &declaration;

            if (std::filesystem::is_directory(path)) {
                std::string error;
                auto        manifest = detail::readManifest(path, error);
                if (!manifest) {
                    report("跳过 " + shown + "：" + error, ConsoleColor::Red);
                    return std::nullopt;
                }
                if (!std::filesystem::is_regular_file(manifest->libraryPath)) {
                    report(
                        "跳过 " + manifest->id + "：清单指向的产物不存在 " + manifest->libraryPath.generic_string(),
                        ConsoleColor::Red
                    );
                    return std::nullopt;
                }
                // 清单里写了 ABI 要求就先校一道 —— 比载进来再发现不匹配便宜得多。
                if (manifest->abiMajor != 0 && manifest->abiMajor != MCDK_ABI_VERSION_MAJOR) {
                    report(
                        "跳过 " + manifest->id + "：清单声明 ABI 主版本 " + std::to_string(manifest->abiMajor)
                            + "，宿主提供 " + std::to_string(MCDK_ABI_VERSION_MAJOR),
                        ConsoleColor::Red
                    );
                    return std::nullopt;
                }
                if (manifest->abiMinor > MCDK_ABI_VERSION_MINOR) {
                    report(
                        "跳过 " + manifest->id + "：清单需要 ABI 次版本 " + std::to_string(manifest->abiMinor)
                            + "，宿主只有 " + std::to_string(MCDK_ABI_VERSION_MINOR) + "，请升级 mcdk",
                        ConsoleColor::Red
                    );
                    return std::nullopt;
                }
                if (!declaration.id.empty() && declaration.id != manifest->id) {
                    report(
                        "跳过 " + shown + "：id 不符（声明 " + declaration.id + "，清单 " + manifest->id
                            + "），内容可能已被替换",
                        ConsoleColor::Red
                    );
                    return std::nullopt;
                }
                candidate.library  = manifest->libraryPath;
                candidate.manifest = std::move(manifest);
                return candidate;
            }

            if (!std::filesystem::is_regular_file(path)) {
                report("跳过 " + shown + "：文件不存在", ConsoleColor::Yellow);
                return std::nullopt;
            }
            // 直指动态库：跳过清单，仅建议用于本地开发调试（06-loading.md §2.2 第 5 条）。
            candidate.library = path;
            return candidate;
        }

        // 声明集内的依赖拓扑排序。返回可加载的顺序；依赖缺失或成环的项被剔除。
        [[nodiscard]] static std::vector<const Candidate*> topologicalOrder(const std::vector<Candidate>& candidates) {
            // 先按 priority 稳定排序：拓扑排序只约束有依赖关系的那些对，
            // 其余顺序由用户的 priority 与声明顺序决定。
            std::vector<const Candidate*> pending;
            pending.reserve(candidates.size());
            for (const auto& candidate : candidates) {
                pending.push_back(&candidate);
            }
            std::stable_sort(pending.begin(), pending.end(), [](const Candidate* left, const Candidate* right) {
                return left->declaration->priority < right->declaration->priority;
            });

            std::unordered_map<std::string, const Candidate*> byId;
            for (const auto* candidate : pending) {
                if (candidate->manifest) {
                    byId.emplace(candidate->manifest->id, candidate);
                }
            }

            // 依赖不在已启用集合内 —— 报错并跳过。**禁止**自动去别处寻找该依赖：
            // 那等于绕过了用户显式声明这一信任前提（06-loading.md §1）。
            std::unordered_set<const Candidate*> rejected;
            for (const auto* candidate : pending) {
                if (!candidate->manifest) {
                    continue;
                }
                for (const auto& dependency : candidate->manifest->dependencies) {
                    const auto it = byId.find(dependency.id);
                    if (it == byId.end()) {
                        report(
                            "跳过 " + candidate->manifest->id + "：依赖 " + dependency.id
                                + " 不在已启用的声明集合内，请先把它加进 .mcdev.json",
                            ConsoleColor::Red
                        );
                        rejected.insert(candidate);
                        continue;
                    }
                    const auto& provided = it->second->manifest->version;
                    if (!detail::versionSatisfies(provided, dependency.versionSpec)) {
                        report(
                            "跳过 " + candidate->manifest->id + "：依赖 " + dependency.id + " 需要 "
                                + dependency.versionSpec + "，实际为 " + provided,
                            ConsoleColor::Red
                        );
                        rejected.insert(candidate);
                    }
                }
            }

            // Kahn。每轮取出依赖已全部就绪的项，保持 pending 的相对顺序。
            std::vector<const Candidate*>   result;
            std::unordered_set<std::string> satisfied;
            std::vector<const Candidate*>   remaining;
            for (const auto* candidate : pending) {
                if (rejected.count(candidate) == 0) {
                    remaining.push_back(candidate);
                }
            }
            while (!remaining.empty()) {
                std::vector<const Candidate*> next;
                bool                          progressed = false;
                for (const auto* candidate : remaining) {
                    const bool ready =
                        !candidate->manifest
                        || std::all_of(
                            candidate->manifest->dependencies.begin(),
                            candidate->manifest->dependencies.end(),
                            [&satisfied](const detail::PluginDependency& dependency) {
                                return satisfied.count(dependency.id) != 0;
                            }
                        );
                    if (ready) {
                        result.push_back(candidate);
                        progressed = true;
                    } else {
                        next.push_back(candidate);
                    }
                }
                // 同一轮内统一补登记，避免同轮内部产生顺序依赖。
                for (const auto* candidate : result) {
                    if (candidate->manifest) {
                        satisfied.insert(candidate->manifest->id);
                    }
                }
                if (!progressed) {
                    // 成环：整个环中的插件全部跳过，而不是挑一个进去。
                    std::string names;
                    for (const auto* candidate : next) {
                        names += (names.empty() ? "" : ", ") + candidate->displayName();
                    }
                    report("跳过依赖成环的插件：" + names, ConsoleColor::Red);
                    break;
                }
                remaining = std::move(next);
            }
            return result;
        }

        [[nodiscard]] static bool loadOne(const Candidate& candidate) {
            const auto& declaration = *candidate.declaration;
            const auto& path        = candidate.library;
            const auto  shown       = path.generic_string();

            std::string error;
            void*       module = loadModule(path, error);
            if (module == nullptr) {
                report("跳过 " + shown + "：" + error, ConsoleColor::Red);
                return false;
            }

            const std::string entrySymbol =
                candidate.manifest && !candidate.manifest->entrySymbol.empty() ? candidate.manifest->entrySymbol
                                                                              : std::string(MCDK_PLUGIN_ENTRY_SYMBOL);
            auto* symbol = findSymbol(module, entrySymbol.c_str());
            if (symbol == nullptr) {
                report("跳过 " + shown + "：找不到入口符号 " + entrySymbol, ConsoleColor::Red);
                return false;
            }
            const auto entry = reinterpret_cast<mcdk_plugin_entry_fn>(symbol);

            // 句柄必须在调用入口之前就存在：host_info.self 要带给插件。
            detail::PluginRecord placeholder;
            placeholder.path       = path;
            placeholder.module     = module;
            placeholder.configJson = declaration.configJson;
            const auto handle      = detail::registry().add(std::move(placeholder));

            mcdk_host_info info{};
            info.struct_size   = static_cast<std::uint32_t>(sizeof(mcdk_host_info));
            info.abi_major     = MCDK_ABI_VERSION_MAJOR;
            info.abi_minor     = MCDK_ABI_VERSION_MINOR;
            info.host_version  = toAbi(kHostVersion);
            info.self          = handle;
            info.get_interface = &getInterfaceThunk;

            mcdk_plugin_desc desc{};
            // 插件侧的入口是 noexcept 的（SDK 屏障保证），这里不需要 try。
            if (entry(&info, &desc) != MCDK_TRUE) {
                detail::registry().retire(handle);
                report("跳过 " + shown + "：入口函数报告初始化失败", ConsoleColor::Red);
                return false;
            }

            if (desc.struct_size < sizeof(mcdk_plugin_desc)) {
                detail::registry().retire(handle);
                report("跳过 " + shown + "：plugin_desc 过小，插件可能由更旧的 SDK 构建", ConsoleColor::Red);
                return false;
            }
            if (desc.abi_major != MCDK_ABI_VERSION_MAJOR) {
                detail::registry().retire(handle);
                report(
                    "跳过 " + shown + "：ABI 主版本不符（插件 " + std::to_string(desc.abi_major) + "，宿主 "
                        + std::to_string(MCDK_ABI_VERSION_MAJOR) + "）",
                    ConsoleColor::Red
                );
                return false;
            }
            if (desc.abi_minor > MCDK_ABI_VERSION_MINOR) {
                detail::registry().retire(handle);
                report(
                    "跳过 " + shown + "：插件需要 ABI 次版本 " + std::to_string(desc.abi_minor) + "，宿主只有 "
                        + std::to_string(MCDK_ABI_VERSION_MINOR) + "，请升级 mcdk",
                    ConsoleColor::Red
                );
                return false;
            }

            // 全部深拷贝：插件给的 mcdk_str 按借用规则处理，不保存其指针。
            auto id      = toString(desc.id);
            auto name    = toString(desc.name);
            auto version = toString(desc.version);
            if (id.empty()) {
                detail::registry().retire(handle);
                report("跳过 " + shown + "：插件未报告 id", ConsoleColor::Red);
                return false;
            }
            // 清单声明的 id 必须与二进制实际报出的一致。不校的话，依赖图就是按一组
            // 与运行时无关的 id 排的序——排出来的顺序看着对，实际上没有任何保障。
            if (candidate.manifest && candidate.manifest->id != id) {
                detail::registry().retire(handle);
                report(
                    "跳过 " + shown + "：清单声明 id 为 " + candidate.manifest->id + "，二进制实际报出 " + id,
                    ConsoleColor::Red
                );
                return false;
            }
            // 声明里写了期望 id 就校验，防止 path 指向的内容被换成另一个插件。
            if (!declaration.id.empty() && declaration.id != id) {
                detail::registry().retire(handle);
                report(
                    "跳过 " + shown + "：id 不符（声明 " + declaration.id + "，实际 " + id + "），内容可能已被替换",
                    ConsoleColor::Red
                );
                return false;
            }

            auto* record = detail::registry().find(handle);
            if (record == nullptr) {
                return false;
            }
            record->id       = std::move(id);
            record->name     = std::move(name);
            record->version  = std::move(version);
            record->desc     = desc;
            record->abiMajor = desc.abi_major;
            record->abiMinor = desc.abi_minor;
            if (candidate.manifest) {
                record->mcpTools = candidate.manifest->mcpTools;
            }

            report(
                record->id + " " + record->version + "  (abi " + std::to_string(desc.abi_major) + "."
                    + std::to_string(desc.abi_minor) + ")",
                ConsoleColor::Green
            );
            return true;
        }
    };

    Host::Host(ConsoleOutputCallback output) : mImpl(std::make_unique<Impl>(std::move(output))) {}

    Host::~Host() {
        if (!mImpl) {
            return;
        }
        try {
            mImpl->shutdown();
        } catch (...) {
            // 析构必须保持 noexcept。
        }
    }

    void Host::declareMcpTools(runtime::McpToolRegistry& registry) { mImpl->declareMcpTools(registry); }

    void
    Host::loadDeclared(const std::vector<PluginDeclaration>& declarations, const std::filesystem::path& baseDirectory) {
        mImpl->loadDeclared(declarations, baseDirectory);
    }

    void Host::advance(mcdk_stage stage) { mImpl->advance(stage); }

    void Host::shutdown() { mImpl->shutdown(); }

    bool Host::empty() const noexcept { return mImpl->empty(); }

    std::vector<LoadedPlugin> Host::loaded() const { return mImpl->loaded(); }

    Host& instance() {
        static Host host(&printColoredAtomic);
        return host;
    }

} // namespace mcdk::plugin_host
