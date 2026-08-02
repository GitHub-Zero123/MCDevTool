#include <rpc_registry.hpp>

#include <iostream>
#include <string>

namespace {
    bool expect(bool condition, const char* description) {
        if (!condition) {
            std::cerr << "Failed: " << description << '\n';
        }
        return condition;
    }
}

int main() {
    mcdk::RpcRegistry registry;

    const auto handler = [](const mcdk::RpcContext&, const nlohmann::json& params) -> mcdk::RpcResult {
        return nlohmann::json{{"echo", params}};
    };

    bool passed = true;
    passed &= expect(
        !registry.bindRaw({.name = "invalid"}, {}, handler)
            && registry.bindRaw({.name = "invalid"}, {}, handler).error() == mcdk::RpcBindError::InvalidName,
        "method names require a namespace"
    );

    auto first = registry.bindRaw({.name = "zeta/value"}, {}, handler);
    passed &= expect(first.has_value(), "valid method registration");
    passed &= expect(
        registry.bindRaw({.name = "zeta/value"}, {}, handler).error() == mcdk::RpcBindError::DuplicateName,
        "duplicate method registration"
    );
    passed &= expect(
        registry.bindRaw({.name = "alpha/value"}, {}, handler).has_value(),
        "second valid method registration"
    );
    passed &= expect(
        registry.bind<nlohmann::json, int>(
            {.name = "typed/direct"},
            {},
            [](const mcdk::RpcContext&, const nlohmann::json& params) { return params.value("value", 0); }
        ).has_value(),
        "typed handler with direct result"
    );
    passed &= expect(
        registry.bindNotification<nlohmann::json>(
            {.name = "typed/notify"},
            {},
            [](const mcdk::RpcContext&, const nlohmann::json&) {}
        ).has_value(),
        "typed notification handler with void result"
    );

    registry.seal();
    passed &= expect(registry.sealed(), "registry is sealed");
    passed &= expect(
        registry.bindRaw({.name = "later/value"}, {}, handler).error() == mcdk::RpcBindError::RegistrySealed,
        "sealed registry rejects registration"
    );

    const auto description = registry.describeMethods();
    passed &= expect(description["methods"].size() == 4, "method discovery count");
    passed &= expect(description["methods"][0]["name"] == "alpha/value", "method discovery sorting");
    passed &= expect(description["methods"][3]["name"] == "zeta/value", "method discovery sorting tail");

    const auto* entry = registry.find("zeta/value");
    passed &= expect(entry != nullptr, "sealed registry lookup");
    if (entry != nullptr) {
        const mcdk::RpcContext context{
            .method       = "zeta/value",
            .notification = false,
            .deadline     = std::chrono::steady_clock::now() + std::chrono::seconds(1),
            .sessionId    = "test-session",
            .cancelled    = std::make_shared<std::atomic<bool>>(false),
        };
        auto result = entry->handler(context, {{"value", 42}});
        passed &= expect(result && (*result)["echo"]["value"] == 42, "registered handler invocation");
    }

    const auto* typedEntry = registry.find("typed/direct");
    if (typedEntry != nullptr) {
        const mcdk::RpcContext context{
            .method       = "typed/direct",
            .notification = false,
            .deadline     = std::chrono::steady_clock::now() + std::chrono::seconds(1),
            .sessionId    = "test-session",
            .cancelled    = std::make_shared<std::atomic<bool>>(false),
        };
        auto result = typedEntry->handler(context, {{"value", 7}});
        passed &= expect(result && *result == 7, "typed direct-result handler invocation");
    } else {
        passed = false;
    }

    return passed ? 0 : 1;
}
