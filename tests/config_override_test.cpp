#include <mcdk/config.hpp>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace {
    using Json   = nlohmann::json;
    namespace fs = std::filesystem;

    void require(bool condition, const char* message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    bool throws(const std::function<void()>& action) {
        try {
            action();
        } catch (const std::exception&) {
            return true;
        }
        return false;
    }

    void testValueTypes() {
        Json root = Json::object();
        mcdk::applyConfigOverride(root, "reset_world=true");
        mcdk::applyConfigOverride(root, "world_seed=42");
        mcdk::applyConfigOverride(root, "skin=null");
        mcdk::applyConfigOverride(root, "dirs=[\"./\",\"D:/x\"]");
        mcdk::applyConfigOverride(root, "world_name=冒烟测试");
        mcdk::applyConfigOverride(root, "quoted=\"123\"");
        mcdk::applyConfigOverride(root, "empty=");
        mcdk::applyConfigOverride(root, "expr=a=b");

        require(root["reset_world"] == true, "boolean value");
        require(root["world_seed"] == 42, "number value");
        require(root["skin"].is_null(), "null value");
        require(root["dirs"].size() == 2, "array value");
        require(root["world_name"] == "冒烟测试", "plain text stays a string");
        require(root["quoted"] == "123", "JSON string value");
        require(root["empty"] == "", "empty value");
        require(root["expr"] == "a=b", "only the first = separates");
    }

    void testNestedKeys() {
        Json root = {{"mcp_server_config", {{"enabled", true}}}, {"world_seed", nullptr}};
        mcdk::applyConfigOverride(root, "mcp_server_config.server_port=19140");
        mcdk::applyConfigOverride(root, "a.b.c=1");
        mcdk::applyConfigOverride(root, "world_seed.x=1");

        require(root["mcp_server_config"]["enabled"] == true, "siblings are kept");
        require(root["mcp_server_config"]["server_port"] == 19140, "nested value");
        require(root["a"]["b"]["c"] == 1, "missing levels are created");
        require(root["world_seed"]["x"] == 1, "null level becomes an object");
    }

    void testInvalidExpressions() {
        Json root = {{"world_name", "x"}};
        for (const auto* expression : {"world_name", "=1", ".a=1", "a..b=1", "a.=1", "world_name.x=1"}) {
            require(throws([&] { mcdk::applyConfigOverride(root, expression); }), expression);
        }
    }

    void testLoadFromUnicodePath() {
        const auto directory = fs::temp_directory_path() / fs::u8path("mcdk_配置_测试");
        fs::create_directories(directory);
        const auto path = directory / fs::u8path("冒烟.json");
        {
            std::ofstream output(path, std::ios::binary);
            output << "// 允许注释\n{\"world_name\": \"原名\", \"reset_world\": false}";
        }

        const auto config = mcdk::loadUserConfig(path, {"world_name=覆写", "reset_world=true"});
        require(config.world.name == "覆写", "override reaches the parsed config");
        require(config.world.reset, "boolean override reaches the parsed config");

        const auto before = fs::last_write_time(path);
        (void)mcdk::loadUserConfig(path);
        require(fs::last_write_time(path) == before, "loading never writes the file");

        fs::remove_all(directory);
    }

    void testLoadFailures() {
        const auto directory = fs::temp_directory_path() / "mcdk_config_failures";
        fs::create_directories(directory);
        const auto missing = directory / "missing.json";
        require(throws([&] { (void)mcdk::loadUserConfig(missing); }), "missing file");
        require(!fs::exists(missing), "missing file is not created");

        const auto array = directory / "array.json";
        std::ofstream(array, std::ios::binary) << "[]";
        require(throws([&] { (void)mcdk::loadUserConfig(array); }), "non-object root");

        const auto wrongType = directory / "wrong.json";
        std::ofstream(wrongType, std::ios::binary) << "{}";
        require(throws([&] { (void)mcdk::loadUserConfig(wrongType, {"world_name=1"}); }), "wrong value type");

        fs::remove_all(directory);
    }
} // namespace

int main() {
    try {
        testValueTypes();
        testNestedKeys();
        testInvalidExpressions();
        testLoadFromUnicodePath();
        testLoadFailures();
    } catch (const std::exception& exception) {
        std::cerr << "FAILED: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "config override tests passed\n";
    return EXIT_SUCCESS;
}
