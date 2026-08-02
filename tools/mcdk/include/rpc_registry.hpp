#pragma once

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

namespace mcdk {

    struct RpcError {
        int            code    = -32603;
        std::string    message = "Internal error";
        nlohmann::json data    = nlohmann::json::object();
    };

    using RpcResult = std::expected<nlohmann::json, RpcError>;

    enum class RpcMode : std::uint8_t {
        Request      = 1,
        Notification = 2,
    };

    using RpcModes = std::uint8_t;

    [[nodiscard]] constexpr RpcModes rpcModeMask(RpcMode mode) noexcept {
        return static_cast<RpcModes>(mode);
    }

    [[nodiscard]] constexpr RpcModes operator|(RpcMode left, RpcMode right) noexcept {
        return rpcModeMask(left) | rpcModeMask(right);
    }

    [[nodiscard]] constexpr bool rpcModesContain(RpcModes modes, RpcMode mode) noexcept {
        return (modes & rpcModeMask(mode)) != 0;
    }

    enum class RpcExecutionPolicy {
        Inline,
        Worker,
        GameSerial,
    };

    enum class GameAvailability {
        None,
        DebugEnabled,
        InWorld,
    };

    struct RpcMethodOptions {
        RpcModes                  modes            = rpcModeMask(RpcMode::Request);
        RpcExecutionPolicy       execution        = RpcExecutionPolicy::Worker;
        GameAvailability         gameAvailability = GameAvailability::None;
        std::chrono::milliseconds timeout{10000};
        std::uint32_t            maxConcurrency = 8;
    };

    struct RpcMethodDescriptor {
        std::string    name;
        nlohmann::json paramsSchema = nullptr;
        nlohmann::json resultSchema = nullptr;
    };

    struct RpcContext {
        std::string_view                                  method;
        nlohmann::json                                    id = nullptr;
        bool                                              notification = false;
        std::chrono::steady_clock::time_point             deadline;
        std::string_view                                  sessionId;
        std::shared_ptr<std::atomic<bool>>                cancelled;

        [[nodiscard]] bool isCancelled() const noexcept {
            return cancelled && cancelled->load(std::memory_order_relaxed);
        }
    };

    using RawRpcHandler = std::function<RpcResult(const RpcContext&, const nlohmann::json&)>;

    enum class RpcBindError {
        InvalidName,
        DuplicateName,
        InvalidOptions,
        EmptyHandler,
        RegistrySealed,
    };

    struct RpcMethodEntry {
        RpcMethodDescriptor descriptor;
        RpcMethodOptions    options;
        RawRpcHandler       handler;
    };

    namespace rpc_registry_detail {
        [[nodiscard]] inline RpcError invalidParamsError(const nlohmann::json::exception& error) {
            return {
                .code    = -32602,
                .message = "Invalid params",
                .data    = {{"code", "INVALID_PARAMS"}, {"detail", error.what()}},
            };
        }
    } // namespace rpc_registry_detail

    class RpcRegistry {
    public:
        RpcRegistry() = default;
        RpcRegistry(const RpcRegistry&)            = delete;
        RpcRegistry& operator=(const RpcRegistry&) = delete;

        [[nodiscard]] std::expected<void, RpcBindError>
        bindRaw(RpcMethodDescriptor descriptor, RpcMethodOptions options, RawRpcHandler handler);

        template <class Params, class Result, class Handler>
        [[nodiscard]] std::expected<void, RpcBindError>
        bind(RpcMethodDescriptor descriptor, RpcMethodOptions options, Handler&& handler) {
            using StoredHandler = std::decay_t<Handler>;
            RawRpcHandler adapter =
                [stored = StoredHandler(std::forward<Handler>(handler))](
                    const RpcContext& context,
                    const nlohmann::json& params
                ) mutable -> RpcResult {
                try {
                    Params typedParams = params.template get<Params>();
                    using HandlerResult = std::invoke_result_t<StoredHandler&, const RpcContext&, Params&>;
                    if constexpr (std::same_as<std::remove_cvref_t<HandlerResult>, std::expected<Result, RpcError>>) {
                        auto result = std::invoke(stored, context, typedParams);
                        if (!result) {
                            return std::unexpected(std::move(result.error()));
                        }
                        return nlohmann::json(std::move(*result));
                    } else {
                        static_assert(
                            std::convertible_to<HandlerResult, Result>,
                            "RPC handler must return Result or std::expected<Result, RpcError>"
                        );
                        return nlohmann::json(Result(std::invoke(stored, context, typedParams)));
                    }
                } catch (const nlohmann::json::exception& error) {
                    return std::unexpected(rpc_registry_detail::invalidParamsError(error));
                }
            };
            return bindRaw(std::move(descriptor), options, std::move(adapter));
        }

        template <class Params, class Handler>
        [[nodiscard]] std::expected<void, RpcBindError>
        bindNotification(RpcMethodDescriptor descriptor, RpcMethodOptions options, Handler&& handler) {
            options.modes      = rpcModeMask(RpcMode::Notification);
            using StoredHandler = std::decay_t<Handler>;
            RawRpcHandler adapter =
                [stored = StoredHandler(std::forward<Handler>(handler))](
                    const RpcContext& context,
                    const nlohmann::json& params
                ) mutable -> RpcResult {
                try {
                    Params typedParams = params.template get<Params>();
                    using HandlerResult = std::invoke_result_t<StoredHandler&, const RpcContext&, Params&>;
                    if constexpr (std::same_as<HandlerResult, void>) {
                        std::invoke(stored, context, typedParams);
                    } else {
                        static_assert(
                            std::same_as<std::remove_cvref_t<HandlerResult>, std::optional<RpcError>>,
                            "Notification handler must return void or std::optional<RpcError>"
                        );
                        auto error = std::invoke(stored, context, typedParams);
                        if (error.has_value()) {
                            return std::unexpected(std::move(*error));
                        }
                    }
                    return nlohmann::json::object();
                } catch (const nlohmann::json::exception& error) {
                    return std::unexpected(rpc_registry_detail::invalidParamsError(error));
                }
            };
            return bindRaw(std::move(descriptor), options, std::move(adapter));
        }

        void               seal();
        [[nodiscard]] bool sealed() const noexcept;

        [[nodiscard]] const RpcMethodEntry* find(std::string_view method) const noexcept;
        [[nodiscard]] nlohmann::json         describeMethods() const;

    private:
        struct TransparentStringHash {
            using is_transparent = void;

            [[nodiscard]] std::size_t operator()(std::string_view value) const noexcept {
                return std::hash<std::string_view>{}(value);
            }

            [[nodiscard]] std::size_t operator()(const std::string& value) const noexcept {
                return operator()(std::string_view(value));
            }
        };

        using MethodMap = std::unordered_map<std::string, RpcMethodEntry, TransparentStringHash, std::equal_to<>>;

        mutable std::mutex mMutex;
        MethodMap          mMethods;
        std::atomic<bool>   mSealed = false;
    };

} // namespace mcdk
