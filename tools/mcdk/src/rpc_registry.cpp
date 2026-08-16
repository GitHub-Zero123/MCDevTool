#include <mcdk/rpc_registry.hpp>

#include <algorithm>
#include <vector>

namespace mcdk {
    namespace {
        [[nodiscard]] bool isValidMethodSegment(std::string_view segment) {
            if (segment.empty() || segment.front() < 'a' || segment.front() > 'z') {
                return false;
            }
            return std::ranges::all_of(segment.substr(1), [](const char character) {
                return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9')
                    || character == '_' || character == '-';
            });
        }

        [[nodiscard]] bool isValidMethodName(std::string_view name) {
            const auto separator = name.find('/');
            if (separator == std::string_view::npos) {
                return false;
            }
            std::size_t begin = 0;
            while (begin <= name.size()) {
                const auto end = name.find('/', begin);
                const auto segment = name.substr(begin, end == std::string_view::npos ? name.size() - begin : end - begin);
                if (!isValidMethodSegment(segment)) {
                    return false;
                }
                if (end == std::string_view::npos) {
                    return true;
                }
                begin = end + 1;
            }
            return false;
        }

        [[nodiscard]] const char* availabilityName(GameAvailability availability) {
            switch (availability) {
            case GameAvailability::DebugEnabled:
                return "debug_enabled";
            case GameAvailability::InWorld:
                return "in_world";
            default:
                return "none";
            }
        }

        [[nodiscard]] nlohmann::json modesJson(RpcModes modes) {
            auto result = nlohmann::json::array();
            if (rpcModesContain(modes, RpcMode::Request)) {
                result.push_back("request");
            }
            if (rpcModesContain(modes, RpcMode::Notification)) {
                result.push_back("notification");
            }
            return result;
        }
    } // namespace

    std::expected<void, RpcBindError>
    RpcRegistry::bindRaw(RpcMethodDescriptor descriptor, RpcMethodOptions options, RawRpcHandler handler) {
        std::lock_guard lock(mMutex);
        if (mSealed.load(std::memory_order_relaxed)) {
            return std::unexpected(RpcBindError::RegistrySealed);
        }
        if (!isValidMethodName(descriptor.name)) {
            return std::unexpected(RpcBindError::InvalidName);
        }
        if (!handler) {
            return std::unexpected(RpcBindError::EmptyHandler);
        }
        constexpr auto supportedModes = rpcModeMask(RpcMode::Request) | rpcModeMask(RpcMode::Notification);
        if (options.modes == 0 || (options.modes & ~supportedModes) != 0 || options.timeout.count() <= 0
            || options.maxConcurrency == 0) {
            return std::unexpected(RpcBindError::InvalidOptions);
        }
        if (mMethods.contains(descriptor.name)) {
            return std::unexpected(RpcBindError::DuplicateName);
        }
        auto name = descriptor.name;
        mMethods.emplace(std::move(name), RpcMethodEntry{std::move(descriptor), options, std::move(handler)});
        return {};
    }

    void RpcRegistry::seal() {
        std::lock_guard lock(mMutex);
        mSealed.store(true, std::memory_order_release);
    }

    bool RpcRegistry::sealed() const noexcept { return mSealed.load(std::memory_order_acquire); }

    const RpcMethodEntry* RpcRegistry::find(std::string_view method) const noexcept {
        if (!sealed()) {
            return nullptr;
        }
        const auto iterator = mMethods.find(method);
        return iterator == mMethods.end() ? nullptr : &iterator->second;
    }

    nlohmann::json RpcRegistry::describeMethods() const {
        std::lock_guard lock(mMutex);
        std::vector<const RpcMethodEntry*> entries;
        entries.reserve(mMethods.size());
        for (const auto& [_, entry] : mMethods) {
            entries.push_back(&entry);
        }
        std::ranges::sort(entries, {}, [](const RpcMethodEntry* entry) { return entry->descriptor.name; });

        auto methods = nlohmann::json::array();
        for (const auto* entry : entries) {
            methods.push_back({
                {"name", entry->descriptor.name},
                {"modes", modesJson(entry->options.modes)},
                {"gameAvailability", availabilityName(entry->options.gameAvailability)},
                {"paramsSchema", entry->descriptor.paramsSchema},
                {"resultSchema", entry->descriptor.resultSchema},
            });
        }
        return {{"methods", std::move(methods)}};
    }

} // namespace mcdk
