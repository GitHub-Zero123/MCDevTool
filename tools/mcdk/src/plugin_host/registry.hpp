#pragma once

//
// 已加载插件的句柄表（宿主内部）。
//
// mcdk_handle 的编码：(generation << 32) | slot。
// 槽位作废时 generation 自增，因此陈旧句柄只会查不到，而不会命中被复用的槽位。
// 这是 docs/plugin-system/03-abi-reference.md §5.3 要求的兜底：句柄作废后
// 宿主必须返回 MCDK_ERR_INVALID_HANDLE 而非崩溃——不能假设插件把自己创建的
// 线程 join 干净了。
//

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <vector>
#include <string>

#include <mcdk/console.hpp>
#include <mcdk/plugin/abi/entry.h>

namespace mcdk::plugin_host::detail {

    struct PluginRecord {
        // 全部深拷贝：插件给的 mcdk_str 按借用规则处理，不保存其指针。
        std::string           id;
        std::string           name;
        std::string           version;
        std::filesystem::path path;
        // .mcdev.json 中该条声明的 config，JSON 文本。未设置时为字面量 "null"。
        // 由宿主持有，生命周期覆盖整个会话。
        std::string configJson = "null";

        void*            module = nullptr; // HMODULE / dlopen 句柄
        mcdk_plugin_desc desc{};
        std::uint32_t    abiMajor = 0;
        std::uint32_t    abiMinor = 0;
        bool             degraded = false;
    };

    class Registry {
    public:
        [[nodiscard]] mcdk_handle add(PluginRecord record);

        // 返回的指针在对应槽位被 retire 之前有效。底层是 deque，追加不会
        // 让已发出的指针失效。
        [[nodiscard]] PluginRecord* find(mcdk_handle handle) noexcept;

        void retire(mcdk_handle handle) noexcept;

        // 按加载顺序 / 逆加载顺序遍历仍存活的记录。
        void forEach(const std::function<void(mcdk_handle, PluginRecord&)>& visitor);
        void forEachReversed(const std::function<void(mcdk_handle, PluginRecord&)>& visitor);

        [[nodiscard]] std::size_t aliveCount() const noexcept;

    private:
        struct Slot {
            std::uint32_t generation = 1;
            bool          alive      = false;
            PluginRecord  record;
        };

        [[nodiscard]] static mcdk_handle   encode(std::size_t slot, std::uint32_t generation) noexcept;
        [[nodiscard]] static std::size_t   slotOf(mcdk_handle handle) noexcept;
        [[nodiscard]] static std::uint32_t generationOf(mcdk_handle handle) noexcept;

        mutable std::mutex mMutex;
        std::deque<Slot>   mSlots;
    };

    // 进程内唯一。接口 shim 是自由函数，只拿得到 mcdk_handle，必须靠它反查。
    [[nodiscard]] Registry& registry() noexcept;

    // 宿主的控制台输出回调，由 Host 构造时注入。
    void                                       setOutputCallback(ConsoleOutputCallback callback);
    [[nodiscard]] const ConsoleOutputCallback& outputCallback() noexcept;

    // 宿主版本串与当前生命周期阶段，供 mcdk.core 的 shim 读取。
    [[nodiscard]] const std::string& hostVersion() noexcept;
    void                             setCurrentStage(mcdk_stage stage) noexcept;
    [[nodiscard]] mcdk_stage         currentStage() noexcept;

    // 接口表分发。新增接口表时在 interfaces/ 下加实现并在此登记。
    [[nodiscard]] const void* findInterfaceTable(const char* name, std::uint32_t version) noexcept;

} // namespace mcdk::plugin_host::detail
