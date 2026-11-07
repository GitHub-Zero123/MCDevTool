#include "images.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <utility>

namespace mcdk::plugin_host::detail {

    namespace {

        struct Store {
            std::mutex mutex;
            // key 是图像句柄。value 里带 owner，取用时校验——句柄猜测不应该能
            // 跨插件读到别人的截图。
            struct Entry {
                mcdk_handle                  owner = 0;
                std::shared_ptr<ImageRecord> record;
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
        auto held = std::make_shared<ImageRecord>(std::move(record));

        const std::lock_guard lock(store().mutex);
        const auto            handle = store().nextHandle++;
        store().entries.emplace(handle, Store::Entry{owner, std::move(held)});
        return handle;
    }

    std::shared_ptr<const ImageRecord> findImage(mcdk_handle owner, mcdk_handle image) noexcept {
        const std::lock_guard lock(store().mutex);
        const auto            it = store().entries.find(image);
        if (it == store().entries.end() || it->second.owner != owner) {
            return nullptr;
        }
        // 共享所有权：调用方在锁外用它期间，另一个线程的 release 只会摘掉表项，
        // 不会销毁数据。
        return it->second.record;
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
