#include <mcdk/mc_input_mcp.hpp>
#include <mcdk/log_buffer.hpp>

#include <iostream>
#include <string>

namespace {
    using Json = nlohmann::json;

    bool expect(bool condition, const char* description) {
        if (!condition) {
            std::cerr << "Failed: " << description << '\n';
        }
        return condition;
    }

    // 没有游戏进程时依然可以走完整条校验链路，这正是需要被测试的部分。
    Json call(const Json& arguments, int pid = 0) { return mcdk::mc_input_mcp::handleRequest(pid, arguments); }

    bool hasEnvelope(const Json& result) {
        const auto& body = result["structuredContent"];
        return body.contains("ok") && body.contains("op") && body.contains("data") && body.contains("error")
            && body.contains("warnings") && body.contains("next_calls");
    }

    std::string errorCode(const Json& result) {
        const auto& error = result["structuredContent"]["error"];
        return error.is_object() ? error.value("code", "") : std::string{};
    }

    std::string errorMessage(const Json& result) {
        const auto& error = result["structuredContent"]["error"];
        return error.is_object() ? error.value("message", "") : std::string{};
    }

    bool mentions(const Json& result, std::string_view fragment) {
        return errorMessage(result).find(fragment) != std::string::npos;
    }
} // namespace

int main() {
    bool passed = true;

    // --- 信封与说明 ---

    const auto help  = call(Json{{"op", "/help"}});
    passed          &= expect(!help["isError"].get<bool>(), "/help succeeds without a game process");
    passed          &= expect(hasEnvelope(help), "every result carries the full envelope");
    passed          &= expect(
        help["structuredContent"]["data"].contains("operations"),
        "/help overview lists the available operations"
    );

    const auto keysHelp  = call(Json{{"op", "/help"}, {"args", {{"topic", "keys"}}}});
    passed              &= expect(
        keysHelp["structuredContent"]["data"]["keys"]["names"].dump().find("\"ctrl\"") != std::string::npos,
        "/help topic=keys lists the key names"
    );

    passed &= expect(
        errorCode(call(Json{{"op", "/help"}, {"args", {{"topic", "nope"}}}})) == "INVALID_ARGUMENT",
        "an unknown help topic is rejected"
    );

    // --- 信封层校验 ---

    const auto unknownOp  = call(Json{{"op", "/teleport"}});
    passed               &= expect(errorCode(unknownOp) == "UNKNOWN_OPERATION", "unknown operations are named as such");
    passed &=
        expect(unknownOp["structuredContent"]["next_calls"][0]["op"] == "/help", "unknown operations point at /help");

    passed &= expect(
        errorCode(call(Json{{"op", "/help"}, {"extra", 1}})) == "INVALID_ARGUMENT",
        "unknown top-level fields are rejected"
    );
    passed &= expect(errorCode(call(Json{{"args", Json::object()}})) == "INVALID_ARGUMENT", "op is required");

    // --- 步骤校验：执行之前必须全部通过 ---

    const auto typo =
        call(Json{{"op", "/run"}, {"args", {{"steps", Json::array({Json{{"do", "wait"}, {"milliseconds", 10}}})}}}});
    passed &= expect(errorCode(typo) == "INVALID_ARGUMENT", "a misspelled step field is rejected");
    passed &= expect(mentions(typo, "milliseconds"), "the rejection names the offending field");

    passed &= expect(
        errorCode(call(Json{{"op", "/run"}, {"args", {{"steps", Json::array({Json{{"do", "jump"}}})}}}}))
            == "INVALID_ARGUMENT",
        "an unknown step kind is rejected"
    );

    passed &= expect(
        errorCode(call(Json{{"op", "/run"}, {"args", {{"steps", Json::array({Json{{"do", "wait"}}})}}}}))
            == "INVALID_ARGUMENT",
        "a missing required field is rejected"
    );

    const auto badRange =
        call(Json{{"op", "/run"}, {"args", {{"steps", Json::array({Json{{"do", "wait"}, {"ms", 999999}}})}}}});
    passed &= expect(errorCode(badRange) == "INVALID_ARGUMENT", "out-of-range values are rejected");

    const auto badEnum =
        call(Json{{"op", "/run"}, {"args", {{"steps", Json::array({Json{{"do", "click"}, {"button", "thumb"}}})}}}});
    passed &= expect(mentions(badEnum, "left, right, middle"), "enum rejections list the accepted values");

    const auto badKey = call(
        Json{{"op", "/run"}, {"args", {{"steps", Json::array({Json{{"do", "key"}, {"keys", "ctrl+doesnotexist"}}})}}}}
    );
    passed &= expect(mentions(badKey, "doesnotexist"), "unknown key names are named in the rejection");

    Json many = Json::array();
    for (int index = 0; index < 65; ++index) {
        many.push_back(Json{{"do", "wait"}, {"ms", 1}});
    }
    passed &= expect(
        errorCode(call(Json{{"op", "/run"}, {"args", {{"steps", many}}}})) == "INVALID_ARGUMENT",
        "batches larger than the step cap are rejected"
    );

    passed &= expect(
        errorCode(call(Json{{"op", "/run"}, {"args", {{"steps", Json::array()}}}})) == "INVALID_ARGUMENT",
        "an empty batch is rejected"
    );

    // --- 通过校验之后才谈得上窗口错误 ---

    const auto noWindow = call(Json{{"op", "/state"}});
    passed &= expect(errorCode(noWindow) == "WINDOW_NOT_FOUND", "a missing game process is reported as such");
    passed &=
        expect(noWindow["structuredContent"]["error"]["retryable"].get<bool>(), "a missing game process is retryable");

    // 单步糖与 /run 走同一条通路：校验通过后同样落到窗口检查上。
    const auto sugar  = call(Json{{"op", "/click"}, {"args", {{"at", Json::array({0.5, 0.5})}, {"hold_ms", 200}}}});
    passed           &= expect(errorCode(sugar) == "WINDOW_NOT_FOUND", "single-step sugar reaches the engine");

    passed &= expect(
        errorCode(call(Json{{"op", "/click"}, {"args", {{"atx", 0.5}}}})) == "INVALID_ARGUMENT",
        "single-step sugar validates its own fields"
    );

    // 长按与拖拽的完整参数必须被接受，否则只会在运行期才发现。
    const auto holdAndDrag = call(
        Json{
            {"op", "/run"},
            {"args",
             {{"steps",
               Json::array(
                   {Json{{"do", "key"}, {"keys", "w"}, {"action", "down"}},
                    Json{{"do", "wait"}, {"ms", 500}},
                    Json{{"do", "key"}, {"keys", "w"}, {"action", "up"}},
                    Json{{"do", "click"}, {"hold_ms", 1500}},
                    Json{
                        {"do", "drag"},
                        {"from", Json::array({0.31, 0.55})},
                        {"to", Json::array({0.62, 0.55})},
                        {"hold_ms", 80},
                        {"segments", 12},
                        {"modifiers", "shift"}
                    },
                    Json{{"do", "look"}, {"by", Json::array({120, -40})}},
                    Json{{"do", "scroll"}, {"amount", -2}},
                    Json{{"do", "text"}, {"value", "/gamemode creative"}}}
               )},
              {"leave_held", true},
              {"dry_run", true}}}
        }
    );
    passed &= expect(errorCode(holdAndDrag) == "WINDOW_NOT_FOUND", "a full hold/drag batch passes validation");

    // --- 结束附带日志：窗口失败仍能读取日志，不改变原有错误语义 ---
    mcdk::LogBuffer logs;
    for (int index = 0; index < 25; ++index) {
        logs.add("line " + std::to_string(index));
    }
    Json       logArgs{{"ms", 0}, {"logs", "end"}, {"logs_max_count", 2}};
    const auto withLogs = [&](const Json& args) {
        return mcdk::mc_input_mcp::handleRequest(0, Json{{"op", "/wait"}, {"args", args}}, &logs);
    };
    const auto logged     = withLogs(logArgs);
    passed               &= expect(errorCode(logged) == "WINDOW_NOT_FOUND", "logs preserve the input failure");
    const auto& attached  = logged["structuredContent"]["error"]["progress"]["logs"];
    passed               &= expect(attached["available"] == true, "the configured log buffer is available");
    passed               &= expect(
        attached["entries"] == Json::array({"line 23", "line 24"}) && attached["order"] == "asc",
        "logs return the requested tail in chronological order"
    );
    passed &= expect(
        logged["content"].back()["text"].get<std::string>().find("line 23\nline 24") != std::string::npos,
        "logs are also readable by clients that only consume text content"
    );
    logArgs.erase("logs_max_count");
    const auto defaultLogs  = withLogs(logArgs);
    passed                 &= expect(
        defaultLogs["structuredContent"]["error"]["progress"]["logs"]["entries"].size() == 20,
        "the default log limit is 20"
    );
    const auto unavailable  = call(Json{{"op", "/wait"}, {"args", logArgs}});
    passed                 &= expect(
        unavailable["structuredContent"]["error"]["progress"]["logs"]["available"] == false,
        "an absent log buffer is reported without replacing the input error"
    );
    logs.clear();
    const auto emptyLogs  = withLogs(logArgs);
    passed               &= expect(
        emptyLogs["structuredContent"]["error"]["progress"]["logs"]["entries"].empty(),
        "an empty log buffer produces an empty list"
    );
    logArgs["dry_run"]  = true;
    passed             &= expect(
        !withLogs(logArgs)["structuredContent"]["error"]["progress"].contains("logs"),
        "dry_run does not attach logs"
    );
    logArgs.erase("dry_run");
    logArgs["logs"]  = "none";
    passed          &= expect(
        !withLogs(logArgs)["structuredContent"]["error"]["progress"].contains("logs"),
        "logs=none leaves the existing response unchanged"
    );
    logArgs["logs"] = "end";
    for (const auto& value : Json::array({0, 201, "20"})) {
        logArgs["logs_max_count"] = value;
        passed &= expect(errorCode(withLogs(logArgs)) == "INVALID_ARGUMENT", "invalid log limits are rejected");
    }
    logArgs.erase("logs_max_count");
    logArgs["logs"]  = "invalid";
    passed          &= expect(errorCode(withLogs(logArgs)) == "INVALID_ARGUMENT", "unknown log modes are rejected");

    std::cout << (passed ? "mc_input tests passed" : "mc_input tests failed") << '\n';
    return passed ? 0 : 1;
}
