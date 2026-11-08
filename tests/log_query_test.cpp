#include <mcdk/log_buffer.hpp>
#include <mcdk/log_query.hpp>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
    using Json = nlohmann::json;

    void require(bool condition, const std::string& message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    Json page(const mcdk::LogBuffer& logs, Json params) {
        params["format"]  = "json";
        const auto result = mcdk::log_query::readLogs(logs, logs, params);
        require(!result.value("isError", false), "query failed: " + result.dump());
        return Json::parse(result["content"][0]["text"].get<std::string>());
    }

    void testCursor() {
        mcdk::LogBuffer logs(10, 5);
        for (int i = 1; i <= 4; ++i) {
            logs.add("line " + std::to_string(i));
        }
        auto first = page(logs, {{"limit", 2}});
        require(first["lines"].size() == 2 && first["lines"][0]["text"] == "line 3", "no cursor returns the newest");
        require(first["next"] == 5, "cursor points past the newest line");

        logs.add("line 5");
        auto next = page(logs, {{"since", first["next"]}});
        require(next["lines"].size() == 1 && next["lines"][0]["text"] == "line 5", "cursor returns only new lines");

        auto paged = page(logs, {{"since", 1}, {"limit", 2}});
        require(paged["more"] == true && paged["next"] == 3, "limit stops at a resumable cursor");
    }

    void testSkippedAndClear() {
        mcdk::LogBuffer logs(4, 2);
        for (int i = 1; i <= 5; ++i) {
            logs.add("x" + std::to_string(i));
        }
        // 第 5 行触发淘汰，前两行没了
        auto result = page(logs, {{"since", 1}});
        require(result["skipped"] == 2 && result["lines"][0]["text"] == "x3", "evicted lines are reported");

        logs.clear();
        logs.add("after");
        result = page(logs, {{"since", 6}});
        require(result["lines"].size() == 1 && result["lines"][0]["seq"] == 6, "sequence survives clear");

        result = page(logs, {{"since", 999}});
        require(result["lines"].size() == 1, "a cursor from an older run reads from the start");
    }

    // 旧工具 get_latest_logs / get_log_range 的语义保持不变。
    void testLegacyAccessors() {
        mcdk::LogBuffer logs(10, 5);
        for (int i = 1; i <= 5; ++i) {
            logs.add(std::to_string(i));
        }
        const auto latest = logs.getLatest(2);
        require(latest.size() == 2 && latest[0] == "4" && latest[1] == "5", "getLatest keeps oldest-first order");
        require(logs.getLatest(99).size() == 5, "getLatest caps at size");
        const auto range = logs.getRange(1, 3);
        require(range.size() == 2 && range[0] == "3" && range[1] == "4", "getRange indexes from the newest");
        require(logs.getRangeReversed(0, 2) == std::vector<std::string>{"5", "4"}, "getRangeReversed");
        require(logs.getRange(3, 9).empty(), "out of range is empty");
    }

    void testFilter() {
        mcdk::LogBuffer logs;
        logs.add("[INFO] ready");
        logs.add("[ERROR] Boom");
        logs.add("[INFO] boom later");
        require(page(logs, {{"contains", "boom"}})["lines"].size() == 1, "contains is case-sensitive");
        require(page(logs, {{"contains", "boom"}, {"ignore_case", true}})["lines"].size() == 2, "ignore_case");
        require(page(logs, {{"regex", "^\\[ERROR\\]"}})["lines"].size() == 1, "regex");
        const auto bad = mcdk::log_query::readLogs(logs, logs, {{"regex", "("}});
        require(bad.value("isError", false), "invalid regex is a tool error");
    }
} // namespace

int main() {
    try {
        testCursor();
        testSkippedAndClear();
        testFilter();
        testLegacyAccessors();
    } catch (const std::exception& exception) {
        std::cerr << "FAILED: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "log query tests passed\n";
    return EXIT_SUCCESS;
}
