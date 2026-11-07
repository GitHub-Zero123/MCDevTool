#include <mcdk/plugin_host/session_binding.hpp>

#include <memory>
#include <mutex>

#include "registry.hpp"

namespace mcdk::plugin_host {

    namespace {
        // 整个会话只写两次（绑定、解绑），却被每一次插件接口调用读到。
        // 所以存的是 shared_ptr 而不是对象本身：按值返回 SessionBinding 会在每次
        std::mutex                            gMutex;
        std::shared_ptr<const SessionBinding> gBinding;
    } // namespace

    void bindSession(SessionBinding binding) {
        auto snapshot = std::make_shared<const SessionBinding>(std::move(binding));

        const std::lock_guard lock(gMutex);
        gBinding = std::move(snapshot);
    }

    void unbindSession() noexcept {
        const std::lock_guard lock(gMutex);
        gBinding.reset();
    }

    namespace detail {

        std::shared_ptr<const SessionBinding> sessionBinding() {
            {
                const std::lock_guard lock(gMutex);
                if (gBinding) {
                    // 共享所有权：调用方用它期间 unbindSession 只会换掉指针，
                    // 不会拆掉它手里这份。
                    return gBinding;
                }
            }
            // 未绑定时给一份空快照，调用方就不必到处判空指针——判的是里面的
            // shared_ptr 成员，语义与「运行期还没就绪」一致。
            static const auto kEmpty = std::make_shared<const SessionBinding>();
            return kEmpty;
        }

    } // namespace detail

} // namespace mcdk::plugin_host
