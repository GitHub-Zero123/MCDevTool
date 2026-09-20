#include <mcdk/plugin_host/host.hpp>

#include <mcdk/console_output.hpp>
#include <mcdk/plugin_host/events.hpp>
#include <mcdk/plugin_host/guard.hpp>

#include "registry.hpp"

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

        // TODO 接到真实的项目版本号上；当前仓库尚无统一的版本常量。
        constexpr const char* kHostVersion = "0.1.0";

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

        // 相对路径以 .mcdev.json 所在目录为基准，而非进程工作目录；
        // 以 ~/ 开头展开为用户主目录。见 docs/plugin-system/06-loading.md §2.2。
        [[nodiscard]] std::filesystem::path
        resolvePath(const std::string& raw, const std::filesystem::path& baseDirectory) {
            if (raw.size() >= 2 && raw[0] == '~' && (raw[1] == '/' || raw[1] == '\\')) {
#ifdef _WIN32
                const char* home = std::getenv("USERPROFILE");
#else
                const char* home = std::getenv("HOME");
#endif
                if (home != nullptr) {
                    return std::filesystem::path(home) / std::filesystem::u8path(raw.substr(2));
                }
            }
            auto path = std::filesystem::u8path(raw);
            if (path.is_absolute()) {
                return path.lexically_normal();
            }
            return (baseDirectory / path).lexically_normal();
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
            std::size_t loaded   = 0;
            std::size_t disabled = 0;
            for (const auto& declaration : declarations) {
                if (!declaration.enabled) {
                    ++disabled;
                    continue;
                }
                if (loadOne(declaration, baseDirectory)) {
                    ++loaded;
                }
            }
            report(
                "已加载 " + std::to_string(loaded) + " 个插件，" + std::to_string(disabled) + " 个已禁用",
                loaded > 0 ? ConsoleColor::Green : ConsoleColor::DarkGray
            );
        }

        void advance(mcdk_stage stage) {
            detail::setCurrentStage(stage);
            // 阶段推进是主线程上的天然抽水点。
            pumpMainThreadWork();
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
            detail::registry().forEachReversed([&](mcdk_handle handle, detail::PluginRecord& record) {
                terminate(handle, record);
            });
            // 全部插件终结之后才停派发线程：终结过程本身可能还要抽主线程队列。
            shutdownEventBus();
        }

        [[nodiscard]] bool empty() const noexcept { return detail::registry().aliveCount() == 0; }

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
            // 5. 回收其遗留的宿主资源（图像句柄等）—— v1 尚无此类资源
            // 6. 作废句柄。此后该插件的任何调用都返回 MCDK_ERR_INVALID_HANDLE 而非崩溃。
            detail::registry().retire(handle);

            // 刻意不 FreeLibrary / dlclose：v1 不做热卸载，进程退出时交给操作系统。
            // 插件静态对象析构、残留线程、两侧 CRT 卸载顺序叠加，主动卸载的崩溃
            // 概率远高于它回收的那点资源。见 03-abi-reference.md §5.4。
            record.module = nullptr;
        }

        [[nodiscard]] static bool
        loadOne(const PluginDeclaration& declaration, const std::filesystem::path& baseDirectory) {
            const auto path  = resolvePath(declaration.path, baseDirectory);
            const auto shown = path.generic_string();

            if (!std::filesystem::is_regular_file(path)) {
                report("跳过 " + shown + "：文件不存在", ConsoleColor::Yellow);
                return false;
            }

            std::string error;
            void*       module = loadModule(path, error);
            if (module == nullptr) {
                report("跳过 " + shown + "：" + error, ConsoleColor::Red);
                return false;
            }

            auto* symbol = findSymbol(module, MCDK_PLUGIN_ENTRY_SYMBOL);
            if (symbol == nullptr) {
                report("跳过 " + shown + "：找不到入口符号 " MCDK_PLUGIN_ENTRY_SYMBOL, ConsoleColor::Red);
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
