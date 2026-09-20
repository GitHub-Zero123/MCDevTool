#include <mcdk/plugin_host/session_binding.hpp>

#include <mutex>

#include "registry.hpp"

namespace mcdk::plugin_host {

    namespace {
        // 读多写极少（整个会话就绑一次、解一次），但读方来自插件的任意线程，
        // 不能靠「反正只写一次」蒙混过去。shared_mutex 在这里已经足够廉价。
        std::mutex     gMutex;
        SessionBinding gBinding;
    } // namespace

    void bindSession(SessionBinding binding) {
        const std::lock_guard lock(gMutex);
        gBinding = std::move(binding);
    }

    void unbindSession() noexcept {
        const std::lock_guard lock(gMutex);
        gBinding = SessionBinding{};
    }

    namespace detail {

        SessionBinding sessionBinding() {
            // 返回副本：shared_ptr 拷贝一份，调用方用的期间对象不会被 unbind 拆掉。
            const std::lock_guard lock(gMutex);
            return gBinding;
        }

    } // namespace detail

} // namespace mcdk::plugin_host
