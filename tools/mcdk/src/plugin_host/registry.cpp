#include "registry.hpp"

#include <atomic>
#include <cstring>
#include <utility>

#include <mcdk/plugin/abi/iface/console.h>
#include <mcdk/plugin/abi/iface/core.h>

namespace mcdk::plugin_host::detail {

    namespace {
        ConsoleOutputCallback gOutputCallback;
        // TODO 接到真实的项目版本号上；当前仓库尚无统一的版本常量。
        const std::string       gHostVersion  = "0.1.0";
        std::atomic<mcdk_stage> gCurrentStage = MCDK_STAGE_REGISTER;
    } // namespace

    mcdk_handle Registry::encode(std::size_t slot, std::uint32_t generation) noexcept {
        return (static_cast<mcdk_handle>(generation) << 32) | static_cast<mcdk_handle>(slot & 0xFFFFFFFFu);
    }

    std::size_t Registry::slotOf(mcdk_handle handle) noexcept { return static_cast<std::size_t>(handle & 0xFFFFFFFFu); }

    std::uint32_t Registry::generationOf(mcdk_handle handle) noexcept {
        return static_cast<std::uint32_t>(handle >> 32);
    }

    mcdk_handle Registry::add(PluginRecord record) {
        const std::lock_guard lock(mMutex);
        // 槽位不复用：进程内插件数量以个位计，换取句柄语义最简单。
        mSlots.push_back(Slot{.generation = 1, .alive = true, .record = std::move(record)});
        return encode(mSlots.size() - 1, mSlots.back().generation);
    }

    PluginRecord* Registry::find(mcdk_handle handle) noexcept {
        if (handle == 0) {
            return nullptr;
        }
        const std::lock_guard lock(mMutex);
        const auto            slot = slotOf(handle);
        if (slot >= mSlots.size()) {
            return nullptr;
        }
        Slot& entry = mSlots[slot];
        if (!entry.alive || entry.generation != generationOf(handle)) {
            return nullptr;
        }
        return &entry.record;
    }

    void Registry::retire(mcdk_handle handle) noexcept {
        if (handle == 0) {
            return;
        }
        const std::lock_guard lock(mMutex);
        const auto            slot = slotOf(handle);
        if (slot >= mSlots.size()) {
            return;
        }
        Slot& entry = mSlots[slot];
        if (entry.generation != generationOf(handle)) {
            return;
        }
        entry.alive = false;
        ++entry.generation;
    }

    void Registry::forEach(const std::function<void(mcdk_handle, PluginRecord&)>& visitor) {
        if (!visitor) {
            return;
        }
        // 先在锁内取一份存活句柄快照，再在锁外回调：回调会调用插件代码，
        // 插件可能反过来调用宿主接口而需要这把锁。
        std::vector<mcdk_handle> handles;
        {
            const std::lock_guard lock(mMutex);
            handles.reserve(mSlots.size());
            for (std::size_t index = 0; index < mSlots.size(); ++index) {
                if (mSlots[index].alive) {
                    handles.push_back(encode(index, mSlots[index].generation));
                }
            }
        }
        for (const auto handle : handles) {
            if (auto* record = find(handle)) {
                visitor(handle, *record);
            }
        }
    }

    void Registry::forEachReversed(const std::function<void(mcdk_handle, PluginRecord&)>& visitor) {
        if (!visitor) {
            return;
        }
        std::vector<mcdk_handle> handles;
        {
            const std::lock_guard lock(mMutex);
            for (std::size_t index = 0; index < mSlots.size(); ++index) {
                if (mSlots[index].alive) {
                    handles.push_back(encode(index, mSlots[index].generation));
                }
            }
        }
        for (auto it = handles.rbegin(); it != handles.rend(); ++it) {
            if (auto* record = find(*it)) {
                visitor(*it, *record);
            }
        }
    }

    std::size_t Registry::aliveCount() const noexcept {
        const std::lock_guard lock(mMutex);
        std::size_t           count = 0;
        for (const auto& slot : mSlots) {
            if (slot.alive) {
                ++count;
            }
        }
        return count;
    }

    Registry& registry() noexcept {
        static Registry instance;
        return instance;
    }

    void setOutputCallback(ConsoleOutputCallback callback) { gOutputCallback = std::move(callback); }

    const ConsoleOutputCallback& outputCallback() noexcept { return gOutputCallback; }

    const std::string& hostVersion() noexcept { return gHostVersion; }

    void setCurrentStage(mcdk_stage stage) noexcept { gCurrentStage.store(stage, std::memory_order_release); }

    mcdk_stage currentStage() noexcept { return gCurrentStage.load(std::memory_order_acquire); }

    const mcdk_iface_console* consoleTable() noexcept;
    const mcdk_iface_core*    coreTable() noexcept;

    const void* findInterfaceTable(const char* name, std::uint32_t version) noexcept {
        if (name == nullptr) {
            return nullptr;
        }
        // 未知名字或版本不匹配返回 NULL，插件据此优雅降级而非判定加载失败。
        if (std::strcmp(name, MCDK_IFACE_CORE_NAME) == 0 && version == MCDK_IFACE_CORE_VERSION) {
            return coreTable();
        }
        if (std::strcmp(name, MCDK_IFACE_CONSOLE_NAME) == 0 && version == MCDK_IFACE_CONSOLE_VERSION) {
            return consoleTable();
        }
        return nullptr;
    }

} // namespace mcdk::plugin_host::detail
