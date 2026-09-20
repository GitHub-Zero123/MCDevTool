#include "images.hpp"

#include <atomic>
#include <map>
#include <mutex>
#include <utility>

namespace mcdk::plugin_host::detail {

    namespace {

        struct Store {
            std::mutex mutex;
            // key 是图像句柄。value 里带 owner，取用时校验——句柄猜测不应该能
            // 跨插件读到别人的截图。
            struct Entry {
                mcdk_handle owner = 0;
                ImageRecord record;
            };
            std::map<mcdk_handle, Entry> entries;
            mcdk_handle                  nextHandle = 1;
        };

        Store& store() {
            static Store instance;
            return instance;
        }

    } // namespace

    mcdk_handle addImage(mcdk_handle owner, ImageRecord record) {
        const std::lock_guard lock(store().mutex);
        const auto            handle = store().nextHandle++;
        store().entries.emplace(handle, Store::Entry{owner, std::move(record)});
        return handle;
    }

    const ImageRecord* findImage(mcdk_handle owner, mcdk_handle image) noexcept {
        const std::lock_guard lock(store().mutex);
        const auto            it = store().entries.find(image);
        if (it == store().entries.end() || it->second.owner != owner) {
            return nullptr;
        }
        // std::map 的节点地址稳定，指针在该项被 erase 之前一直有效。
        return &it->second.record;
    }

    void releaseImage(mcdk_handle owner, mcdk_handle image) noexcept {
        const std::lock_guard lock(store().mutex);
        const auto            it = store().entries.find(image);
        if (it != store().entries.end() && it->second.owner == owner) {
            store().entries.erase(it);
        }
    }

    std::size_t releaseAllImages(mcdk_handle owner) noexcept {
        const std::lock_guard lock(store().mutex);
        std::size_t           removed = 0;
        for (auto it = store().entries.begin(); it != store().entries.end();) {
            if (it->second.owner == owner) {
                it = store().entries.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }
        return removed;
    }

} // namespace mcdk::plugin_host::detail
