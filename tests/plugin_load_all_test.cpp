//
// 「同时加载全部工具链产物」的 harness（docs/plugin-system/09-compatibility.md §1）。
//
// 分别加载只能证明各自能跑。真正会出事的是同时加载：两份不同 CRT 的插件共处一个
// 进程时，分配器错配、符号插入、静态初始化顺序这些问题才会显形，而且它们几乎都是
// 静默的——不崩，只是数据悄悄错。所以这一步必须存在，且必须真的把所有产物一起载进来。
//
// 插件路径来自环境变量 MCDEV_TEST_PLUGIN_PATHS，分号分隔。未设置时退回到本地构建出的
// 那一个，这样本地跑 ctest 也有意义，只是覆盖面只有一种工具链。
//
#include <mcdk/plugin_host/host.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#ifndef MCDEV_TEST_CONFORMANCE_PLUGIN
#error "MCDEV_TEST_CONFORMANCE_PLUGIN must point at the built conformance plugin"
#endif

namespace {

    bool expect(bool condition, const std::string& description) {
        if (!condition) {
            std::cerr << "Failed: " << description << '\n';
        }
        return condition;
    }

    [[nodiscard]] std::vector<std::string> splitPaths(const std::string& value) {
        std::vector<std::string> result;
        std::size_t              start = 0;
        while (start <= value.size()) {
            const auto stop  = value.find(';', start);
            auto       piece = value.substr(start, stop == std::string::npos ? std::string::npos : stop - start);
            // 顺手容忍末尾分号与空白项，CI 里拼这个串很容易多一个分隔符。
            while (!piece.empty() && (piece.front() == ' ' || piece.front() == '"')) {
                piece.erase(piece.begin());
            }
            while (!piece.empty() && (piece.back() == ' ' || piece.back() == '"')) {
                piece.pop_back();
            }
            if (!piece.empty()) {
                result.push_back(std::move(piece));
            }
            if (stop == std::string::npos) {
                break;
            }
            start = stop + 1;
        }
        return result;
    }

} // namespace

int main() {
    using namespace mcdk;

    std::vector<std::string> paths;
    if (const char* fromEnv = std::getenv("MCDEV_TEST_PLUGIN_PATHS"); fromEnv != nullptr && *fromEnv != '\0') {
        paths = splitPaths(fromEnv);
        std::cout << "MCDEV_TEST_PLUGIN_PATHS 提供了 " << paths.size() << " 个插件\n";
    } else {
        paths.push_back(MCDEV_TEST_CONFORMANCE_PLUGIN);
        std::cout << "MCDEV_TEST_PLUGIN_PATHS 未设置，只加载本地构建的那一个"
                     "（覆盖面仅一种工具链）\n";
    }

    bool passed = true;
    passed     &= expect(!paths.empty(), "至少要有一个插件路径");

    std::vector<std::string> output;
    plugin_host::Host        host([&output](const std::string& message, ConsoleColor) {
        output.push_back(message);
        std::cout << message << '\n';
    });

    std::vector<PluginDeclaration> declarations;
    declarations.reserve(paths.size());
    for (std::size_t index = 0; index < paths.size(); ++index) {
        const std::filesystem::path path = std::filesystem::u8path(paths[index]);
        if (!std::filesystem::is_regular_file(path)) {
            std::cerr << "Failed: 插件不存在 " << path.generic_string() << '\n';
            passed = false;
            continue;
        }
        // 每份产物给一个不同的 config，既避免它们在日志里混作一谈，
        // 也顺带继续压「同一二进制多实例」那条路径。
        declarations.push_back(
            PluginDeclaration{
                .enabled    = true,
                .path       = path.generic_string(),
                .id         = {},
                .configJson = R"({"slot":)" + std::to_string(index) + "}",
                .priority   = static_cast<int>(index),
            }
        );
    }

    host.loadDeclared(declarations, std::filesystem::current_path());

    const auto loaded = host.loaded();
    passed &= expect(
        loaded.size() == declarations.size(),
        "每一份工具链产物都加载成功（期望 " + std::to_string(declarations.size()) + "，实际 "
            + std::to_string(loaded.size()) + "）"
    );

    // 走完整个生命周期。真正的交叉污染往往不在加载那一刻，而在之后某次调用里。
    host.advance(MCDK_STAGE_REGISTER);
    host.advance(MCDK_STAGE_CONFIG);
    host.advance(MCDK_STAGE_WORLD);
    host.advance(MCDK_STAGE_RUNTIME);
    host.advance(MCDK_STAGE_SHUTDOWN);
    host.shutdown();

    passed &= expect(host.empty(), "全部插件都被正常终结");

    std::cout << (passed ? "plugin_load_all_test passed\n" : "plugin_load_all_test failed\n");
    return passed ? 0 : 1;
}
