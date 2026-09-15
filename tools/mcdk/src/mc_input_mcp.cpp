#include <mcdk/mc_input_mcp.hpp>

#include <mcdk/mcp_tool_definitions.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <initializer_list>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <base64.hpp>
#include <mcdevtool/style.h>
#include <mcdevtool/window_input.h>

namespace {

    using Json       = nlohmann::json;
    namespace Engine = MCDevTool::Input;

    constexpr std::size_t MaxSteps    = 64;
    constexpr int         MaxWaitMs   = 10000;
    constexpr int         MaxBudgetMs = 120000;

    // --- 失败表示：校验层与引擎层共用同一种形状，渲染层只认这一个类型 ---

    struct Failure {
        std::string code;
        std::string message;
        bool        retryable = false;
        Json        progress  = Json(nullptr);
    };

    template <typename T>
    using Outcome = std::expected<T, Failure>;

    std::unexpected<Failure> invalid(std::string message) {
        return std::unexpected(Failure{"INVALID_ARGUMENT", std::move(message), false, Json(nullptr)});
    }

    // --- 枚举名字表。顺序与引擎枚举严格对应，解析即取下标 ---

    constexpr std::string_view ButtonNames[]{"left", "right", "middle"};
    constexpr std::string_view ActionNames[]{"press", "down", "up", "double"};
    constexpr std::string_view KeyActionNames[]{"press", "down", "up"};
    constexpr std::string_view AxisNames[]{"vertical", "horizontal"};
    constexpr std::string_view FocusNames[]{"auto", "require", "keep"};
    constexpr std::string_view ImeNames[]{"suppress", "keep"};
    constexpr std::string_view CaptureNames[]{"none", "end"};

    std::optional<std::size_t> indexOf(std::span<const std::string_view> options, std::string_view value) {
        const auto found = std::ranges::find(options, value);
        return found == options.end() ? std::nullopt
                                      : std::optional<std::size_t>{static_cast<std::size_t>(found - options.begin())};
    }

    std::string joinNames(std::span<const std::string_view> options) {
        std::string text;
        for (const auto option : options) {
            text += text.empty() ? "" : ", ";
            text += option;
        }
        return text;
    }

    // --- 字段规格：新增一个参数就是加一行，不加一条分支 ---

    enum class FieldType : std::uint8_t {
        Int,      // 整数，受 min/max 约束
        Bool,     //
        Text,     // 非空字符串
        Enum,     // 取值限定于 options
        PointAbs, // [x, y]，客户区百分比 0.0-1.0
        PointRel, // [dx, dy]，相对位移
        Keys,     // "ctrl+r" 或 ["ctrl", "r"]
        Steps,    // 步骤数组，递归校验
    };

    struct FieldSpec {
        std::string_view                  name;
        FieldType                         type;
        bool                              required = false;
        double                            min      = 0.0;
        double                            max      = 0.0;
        std::span<const std::string_view> options{};
        std::string_view                  hint{};
    };

    Outcome<void> validateKeyName(std::string_view name, std::string_view where) {
        return Engine::findKey(name).has_value() ? Outcome<void>{}
                                                 : Outcome<void>{invalid(
                                                       std::string{where} + ": unknown key name \"" + std::string{name}
                                                       + "\". Call /help with topic=keys for the full list."
                                                   )};
    }

    Outcome<void> validateKeys(const Json& value, std::string_view where) {
        const auto validateList = [&](std::span<const std::string> names) {
            Outcome<void> result{};
            for (const auto& name : names) {
                result = result.and_then([&] { return validateKeyName(name, where); });
            }
            return result;
        };

        if (value.is_string()) {
            std::vector<std::string> names;
            std::string              current;
            for (const char character : value.get<std::string>()) {
                (character == '+' ? (names.push_back(std::move(current)), current.clear())
                                  : (void)current.push_back(character));
            }
            names.push_back(std::move(current));
            return validateList(names);
        }
        if (value.is_array() && !value.empty()
            && std::ranges::all_of(value, [](const Json& item) { return item.is_string(); })) {
            return validateList(value.get<std::vector<std::string>>());
        }
        return invalid(std::string{where} + ": expected a key name string such as \"ctrl+r\" or an array of names.");
    }

    Outcome<void> validatePair(const Json& value, std::string_view where) {
        const bool shaped = value.is_array() && value.size() == 2
                         && std::ranges::all_of(value, [](const Json& item) { return item.is_number(); });
        return shaped
                 ? Outcome<void>{}
                 : Outcome<void>{invalid(std::string{where} + ": expected a two-number array such as [0.5, 0.5].")};
    }

    Outcome<void> validateField(const Json& value, const FieldSpec& spec, std::string_view where) {
        const std::string label = std::string{where} + "." + std::string{spec.name};

        switch (spec.type) {
        case FieldType::Int:
            return value.is_number() && value.get<double>() >= spec.min && value.get<double>() <= spec.max
                     ? Outcome<void>{}
                     : Outcome<void>{invalid(
                           label + ": expected a number between " + std::to_string(static_cast<long long>(spec.min))
                           + " and " + std::to_string(static_cast<long long>(spec.max)) + "."
                       )};
        case FieldType::Bool:
            return value.is_boolean() ? Outcome<void>{} : Outcome<void>{invalid(label + ": expected a boolean.")};
        case FieldType::Text:
            return value.is_string() && !value.get<std::string>().empty()
                     ? Outcome<void>{}
                     : Outcome<void>{invalid(label + ": expected a non-empty string.")};
        case FieldType::Enum:
            return value.is_string() && indexOf(spec.options, value.get<std::string>()).has_value()
                     ? Outcome<void>{}
                     : Outcome<void>{invalid(label + ": expected one of " + joinNames(spec.options) + ".")};
        case FieldType::PointAbs:
        case FieldType::PointRel:
            return validatePair(value, label);
        case FieldType::Keys:
            return validateKeys(value, label);
        case FieldType::Steps:
            break;
        }
        return {};
    }

    Outcome<void> validateFields(
        const Json&                             object,
        std::span<const FieldSpec>              fields,
        std::string_view                        where,
        std::initializer_list<std::string_view> extra
    ) {
        if (!object.is_object()) {
            return invalid(std::string{where} + ": expected an object.");
        }

        Outcome<void> result{};
        for (const auto& [key, value] : object.items()) {
            const bool known = std::ranges::contains(extra, std::string_view{key})
                            || std::ranges::find(fields, std::string_view{key}, &FieldSpec::name) != fields.end();
            result = result.and_then([&]() -> Outcome<void> {
                return known ? Outcome<void>{}
                             : Outcome<void>{invalid(std::string{where} + ": unknown field \"" + key + "\".")};
            });
        }
        for (const auto& spec : fields) {
            result = result.and_then([&]() -> Outcome<void> {
                const auto found = object.find(spec.name);
                return found == object.end() ? (spec.required ? Outcome<void>{invalid(
                                                                    std::string{where} + ": missing required field \""
                                                                    + std::string{spec.name} + "\"."
                                                                )}
                                                              : Outcome<void>{})
                                             : validateField(*found, spec, where);
            });
        }
        return result;
    }

    // --- 取值助手：走到这里时字段已经通过校验 ---

    std::vector<Engine::Key> readKeys(const Json& value) {
        std::vector<std::string> names;
        if (value.is_string()) {
            std::string current;
            for (const char character : value.get<std::string>()) {
                (character == '+' ? (names.push_back(std::move(current)), current.clear())
                                  : (void)current.push_back(character));
            }
            names.push_back(std::move(current));
        } else {
            names = value.get<std::vector<std::string>>();
        }

        std::vector<Engine::Key> keys;
        keys.reserve(names.size());
        for (const auto& name : names) {
            keys.push_back(*Engine::findKey(name));
        }
        return keys;
    }

    std::vector<Engine::Key> keysField(const Json& object, std::string_view name) {
        const auto found = object.find(name);
        return found == object.end() ? std::vector<Engine::Key>{} : readKeys(*found);
    }

    Engine::Point pointField(const Json& value) {
        return Engine::Point{value[0].get<double>(), value[1].get<double>()};
    }

    int intField(const Json& object, std::string_view name, int fallback) {
        const auto found = object.find(name);
        return found == object.end() ? fallback : found->get<int>();
    }

    bool boolField(const Json& object, std::string_view name, bool fallback) {
        const auto found = object.find(name);
        return found == object.end() ? fallback : found->get<bool>();
    }

    template <typename T>
    T enumField(const Json& object, std::string_view name, std::span<const std::string_view> options, T fallback) {
        const auto found = object.find(name);
        return found == object.end() ? fallback : static_cast<T>(*indexOf(options, found->get<std::string>()));
    }

    // --- 步骤规格表 ---

    constexpr FieldSpec MoveFields[]{
        {"at", FieldType::PointAbs, true, 0, 0, {}, "目标位置，客户区百分比 [x, y]，0.0-1.0"},
    };

    constexpr FieldSpec ClickFields[]{
        {"at",
         FieldType::PointAbs,
         false,
         0,
         0,
         {},
         "目标位置，客户区百分比 [x, y]；省略则在指针当前位置按下（游戏内视角锁定时用这种形式）"},
        {"button", FieldType::Enum, false, 0, 0, ButtonNames, "鼠标键，默认 left"},
        {"action", FieldType::Enum, false, 0, 0, ActionNames, "press=按下并松开，down=只按下，up=只松开，double=双击"},
        {"hold_ms", FieldType::Int, false, 0, MaxWaitMs, {}, "长按时长，默认 50"},
        {"modifiers", FieldType::Keys, false, 0, 0, {}, "按住的修饰键，例如 shift"},
    };

    constexpr FieldSpec DragFields[]{
        {"from", FieldType::PointAbs, true, 0, 0, {}, "起点，客户区百分比 [x, y]"},
        {"to", FieldType::PointAbs, true, 0, 0, {}, "终点，客户区百分比 [x, y]"},
        {"button", FieldType::Enum, false, 0, 0, ButtonNames, "鼠标键，默认 left"},
        {"hold_ms", FieldType::Int, false, 0, MaxWaitMs, {}, "按下后与松开前的停顿，默认 50"},
        {"segments", FieldType::Int, false, 1, 64, {}, "中间插值点数量，默认 8"},
        {"modifiers", FieldType::Keys, false, 0, 0, {}, "按住的修饰键"},
    };

    constexpr FieldSpec ScrollFields[]{
        {"at", FieldType::PointAbs, false, 0, 0, {}, "滚动前先移动到的位置，客户区百分比 [x, y]"},
        {"amount", FieldType::Int, true, -30, 30, {}, "滚轮格数，正数向上/向右"},
        {"axis", FieldType::Enum, false, 0, 0, AxisNames, "滚动轴，默认 vertical"},
        {"modifiers", FieldType::Keys, false, 0, 0, {}, "按住的修饰键"},
    };

    constexpr FieldSpec LookFields[]{
        {"by", FieldType::PointRel, true, 0, 0, {}, "相对位移 [dx, dy]，单位是鼠标原始计数，不是像素或角度"},
        {"segments", FieldType::Int, false, 1, 64, {}, "分段数量，默认 4"},
    };

    constexpr FieldSpec KeyFields[]{
        {"keys", FieldType::Keys, true, 0, 0, {}, "按键或组合键，例如 \"w\" 或 \"ctrl+r\""},
        {"action", FieldType::Enum, false, 0, 0, KeyActionNames, "press=按下并松开，down=只按下，up=只松开"},
        {"hold_ms", FieldType::Int, false, 0, MaxWaitMs, {}, "长按时长，默认 50"},
        {"repeat", FieldType::Int, false, 1, 20, {}, "重复次数，默认 1"},
    };

    constexpr FieldSpec TextFields[]{
        {"value", FieldType::Text, true, 0, 0, {}, "要输入的文本，按 Unicode 逐字投递"},
    };

    constexpr FieldSpec WaitFields[]{
        {"ms", FieldType::Int, true, 0, MaxWaitMs, {}, "等待毫秒数"},
    };

    Engine::Step parseMove(const Json& step) { return Engine::MoveStep{pointField(step.at("at"))}; }

    Engine::Step parseClick(const Json& step) {
        Engine::ClickStep click;
        const auto        at = step.find("at");
        click.at             = at == step.end() ? std::optional<Engine::Point>{} : std::optional{pointField(*at)};
        click.button         = enumField(step, "button", ButtonNames, Engine::MouseButton::Left);
        click.action         = enumField(step, "action", ActionNames, Engine::PressAction::Press);
        click.holdMs         = intField(step, "hold_ms", 50);
        click.modifiers      = keysField(step, "modifiers");
        return click;
    }

    Engine::Step parseDrag(const Json& step) {
        Engine::DragStep drag;
        drag.from      = pointField(step.at("from"));
        drag.to        = pointField(step.at("to"));
        drag.button    = enumField(step, "button", ButtonNames, Engine::MouseButton::Left);
        drag.holdMs    = intField(step, "hold_ms", 50);
        drag.segments  = intField(step, "segments", 8);
        drag.modifiers = keysField(step, "modifiers");
        return drag;
    }

    Engine::Step parseScroll(const Json& step) {
        Engine::ScrollStep scroll;
        const auto         at = step.find("at");
        scroll.at             = at == step.end() ? std::optional<Engine::Point>{} : std::optional{pointField(*at)};
        scroll.amount         = intField(step, "amount", 0);
        scroll.axis           = enumField(step, "axis", AxisNames, Engine::ScrollAxis::Vertical);
        scroll.modifiers      = keysField(step, "modifiers");
        return scroll;
    }

    Engine::Step parseLook(const Json& step) {
        const auto by = pointField(step.at("by"));
        return Engine::LookStep{by.x, by.y, intField(step, "segments", 4)};
    }

    Engine::Step parseKey(const Json& step) {
        Engine::KeyStep key;
        key.keys   = readKeys(step.at("keys"));
        key.action = enumField(step, "action", KeyActionNames, Engine::PressAction::Press);
        key.holdMs = intField(step, "hold_ms", 50);
        key.repeat = intField(step, "repeat", 1);
        return key;
    }

    Engine::Step parseText(const Json& step) { return Engine::TextStep{step.at("value").get<std::string>()}; }

    Engine::Step parseWait(const Json& step) { return Engine::WaitStep{intField(step, "ms", 0)}; }

    struct StepSpec {
        std::string_view           name;
        std::span<const FieldSpec> fields;
        Engine::Step (*parse)(const Json&);
        std::string_view summary;
    };

    constexpr StepSpec StepSpecs[]{
        {Engine::MoveStep::kName, MoveFields, &parseMove, "移动指针，用于悬停或拖拽前定位"},
        {Engine::ClickStep::kName, ClickFields, &parseClick, "点击/长按/双击，支持修饰键"},
        {Engine::DragStep::kName, DragFields, &parseDrag, "按住并拖动，中途产生插值移动事件"},
        {Engine::ScrollStep::kName, ScrollFields, &parseScroll, "滚轮，用于切换热键栏或滚动列表"},
        {Engine::LookStep::kName, LookFields, &parseLook, "相对移动视角，游戏独占指针时唯一有效的移动方式"},
        {Engine::KeyStep::kName, KeyFields, &parseKey, "按键、组合键与长按"},
        {Engine::TextStep::kName, TextFields, &parseText, "输入文本，走 Unicode 通道"},
        {Engine::WaitStep::kName, WaitFields, &parseWait, "等待，用于让界面动画或游戏刻推进"},
    };

    std::string stepNames() {
        std::string text;
        for (const auto& spec : StepSpecs) {
            text += text.empty() ? "" : ", ";
            text += spec.name;
        }
        return text;
    }

    // --- 批次选项 ---

    constexpr FieldSpec RunOptions[]{
        {"focus", FieldType::Enum, false, 0, 0, FocusNames, "auto=自动置顶，require=不在前台就报错，keep=完全不干预"},
        {"ime",
         FieldType::Enum,
         false,
         0,
         0,
         ImeNames,
         "suppress=批次期间关闭输入法并切到英数（默认，游戏里 WASD 是动作输入而非打字），keep=不干预；"
         "结束后恢复原状。text 步骤走 Unicode 通道，不受此项影响"},
        {"step_delay_ms", FieldType::Int, false, 0, 2000, {}, "步骤之间的间隔，默认 60（约一个游戏刻）"},
        {"budget_ms", FieldType::Int, false, 100, MaxBudgetMs, {}, "整批时间上限，默认 30000"},
        {"restore_cursor", FieldType::Bool, false, 0, 0, {}, "结束后把鼠标放回原处，默认 true"},
        {"restore_if_minimized", FieldType::Bool, false, 0, 0, {}, "窗口最小化时先还原，默认 true"},
        {"leave_held",
         FieldType::Bool,
         false,
         0,
         0,
         {},
         "批次结束后保持按下状态，默认 false；必须用 /release-all 收尾"},
        {"dry_run", FieldType::Bool, false, 0, 0, {}, "只校验并换算坐标，不投递任何输入"},
        {"capture", FieldType::Enum, false, 0, 0, CaptureNames, "end=结束后附带一张截图，默认 none"},
    };

    constexpr FieldSpec StepsField[]{
        {"steps", FieldType::Steps, true, 0, 0, {}, "步骤数组，最多 64 步"},
    };

    Engine::Options readOptions(const Json& args) {
        Engine::Options options;
        options.focus              = enumField(args, "focus", FocusNames, Engine::FocusPolicy::Auto);
        options.ime                = enumField(args, "ime", ImeNames, Engine::ImePolicy::Suppress);
        options.stepDelayMs        = intField(args, "step_delay_ms", 60);
        options.budgetMs           = intField(args, "budget_ms", 30000);
        options.restoreCursor      = boolField(args, "restore_cursor", true);
        options.restoreIfMinimized = boolField(args, "restore_if_minimized", true);
        options.leaveHeld          = boolField(args, "leave_held", false);
        options.dryRun             = boolField(args, "dry_run", false);
        return options;
    }

    bool wantsCapture(const Json& args) {
        const auto found = args.find("capture");
        return found != args.end() && found->get<std::string>() == "end";
    }

    // --- 步骤数组的校验与解析 ---

    const StepSpec* findStepSpec(std::string_view name) {
        const auto found = std::ranges::find(StepSpecs, name, &StepSpec::name);
        return found == std::end(StepSpecs) ? nullptr : &*found;
    }

    Outcome<void> validateStep(const Json& step, std::size_t index) {
        const std::string where = "steps[" + std::to_string(index) + "]";
        if (!step.is_object() || !step.contains("do") || !step["do"].is_string()) {
            return invalid(where + ": every step needs a string \"do\" field. Available: " + stepNames() + ".");
        }

        const auto* spec = findStepSpec(step["do"].get<std::string>());
        return spec == nullptr ? Outcome<void>{invalid(
                                     where + ": unknown step \"" + step["do"].get<std::string>()
                                     + "\". Available: " + stepNames() + "."
                                 )}
                               : validateFields(step, spec->fields, where, {"do"});
    }

    // 全部步骤先整体校验，再开始执行：不存在“跑到第五步才发现字段拼错”的情况。
    Outcome<std::vector<Engine::Step>> readSteps(const Json& args) {
        const auto found = args.find("steps");
        if (found == args.end() || !found->is_array() || found->empty()) {
            return invalid("steps: expected a non-empty array of step objects.");
        }
        if (found->size() > MaxSteps) {
            return invalid("steps: at most " + std::to_string(MaxSteps) + " steps per call.");
        }

        Outcome<void> checked{};
        for (std::size_t index = 0; index < found->size(); ++index) {
            checked = checked.and_then([&] { return validateStep((*found)[index], index); });
        }

        return checked.transform([&] {
            std::vector<Engine::Step> steps;
            steps.reserve(found->size());
            for (const auto& step : *found) {
                steps.push_back(findStepSpec(step["do"].get<std::string>())->parse(step));
            }
            return steps;
        });
    }

    // --- 引擎错误 -> 对外错误 ---

    constexpr Engine::ErrorCode RetryableCodes[]{
        Engine::ErrorCode::WindowNotFound,
        Engine::ErrorCode::WindowMinimized,
        Engine::ErrorCode::FocusDenied,
        Engine::ErrorCode::FocusLost,
        Engine::ErrorCode::Busy,
        Engine::ErrorCode::DeadlineExceeded,
        Engine::ErrorCode::PartialFailure,
    };

    struct Recovery {
        std::string_view code;
        std::string_view op;
        std::string_view reason;
    };

    constexpr Recovery Recoveries[]{
        {"INVALID_ARGUMENT", "/help", "Check the exact field names and ranges before retrying."},
        {"UNKNOWN_OPERATION", "/help", "List the available operations."},
        {"WINDOW_NOT_FOUND", "/state", "Confirm the game window exists before sending input."},
        {"WINDOW_MINIMIZED", "/state", "Confirm the window was restored before sending input."},
        {"FOCUS_DENIED", "/state", "Check which window currently holds the foreground."},
        {"FOCUS_LOST", "/state", "Re-check the window state, then resume from the reported step."},
        {"POINTER_MODE_MISMATCH", "/state", "Read pointer_locked and switch to look or an in-place click."},
        {"BUSY", "/state", "Another batch is running; check state and retry shortly."},
        {"PARTIAL_FAILURE", "/state", "Verify the window state before resuming from the reported step."},
        {"DEADLINE_EXCEEDED", "/help", "Split the work into smaller batches or raise budget_ms."},
    };

    Json pixelJson(const Engine::Pixel& pixel) { return Json{{"x", pixel.x}, {"y", pixel.y}}; }

    Json windowJson(const Engine::WindowInfo& window) {
        return Json{
            {"width", window.width},
            {"height", window.height},
            {"foreground", window.foreground},
            {"pointer_locked", window.pointerLocked},
            {"ime_open", window.imeOpen},
        };
    }

    Json reportJson(const Engine::Report& report) {
        Json steps = Json::array();
        for (const auto& outcome : report.steps) {
            steps.push_back(
                Json{
                    {"index", outcome.index},
                    {"do", outcome.kind},
                    {"at", outcome.at.has_value() ? pixelJson(*outcome.at) : Json(nullptr)},
                    {"elapsed_ms", outcome.elapsedMs},
                }
            );
        }
        return Json{
            {"executed", report.executed},
            {"total", report.total},
            {"dry_run", report.dryRun},
            {"window", windowJson(report.window)},
            {"steps", std::move(steps)},
            {"released", report.released},
            {"held", report.held},
        };
    }

    Failure toFailure(const Engine::Error& error) {
        Json progress = reportJson(error.progress);
        progress["failed_index"] =
            error.progress.executed < error.progress.total ? Json(error.progress.executed) : Json(nullptr);

        return Failure{
            std::string{Engine::errorCodeName(error.code)},
            error.message,
            std::ranges::contains(RetryableCodes, error.code),
            std::move(progress),
        };
    }

    // --- 应答装配 ---

    struct Payload {
        Json                       data     = Json::object();
        Json                       warnings = Json::array();
        std::string                summary;
        std::optional<std::string> image;
    };

    using OpResult = Outcome<Payload>;

    Json nextCallsFor(std::string_view code) {
        const auto found = std::ranges::find(Recoveries, code, &Recovery::code);
        return found == std::end(Recoveries)
                 ? Json::array()
                 : Json::array({Json{{"op", found->op}, {"args", Json::object()}, {"reason", found->reason}}});
    }

    Json envelope(bool ok, std::string_view op, Json data, Json error, Json warnings, Json nextCalls) {
        return Json{
            {"ok", ok},
            {"op", op},
            {"data", std::move(data)},
            {"error", std::move(error)},
            {"warnings", std::move(warnings)},
            {"next_calls", std::move(nextCalls)},
        };
    }

    Json toolResult(Json body, const std::string& summary, const std::optional<std::string>& image) {
        Json content = Json::array({Json{{"type", "text"}, {"text", summary}}});
        if (image.has_value()) {
            content.push_back(Json{{"type", "image"}, {"data", *image}, {"mimeType", "image/jpeg"}});
        }
        return Json{
            {"isError", !body.at("ok").get<bool>()},
            {"content", std::move(content)},
            {"structuredContent", std::move(body)},
        };
    }

    Json renderFailure(std::string_view op, const Failure& failure) {
        Json error = Json{
            {"code", failure.code},
            {"message", failure.message},
            {"retryable", failure.retryable},
            {"progress", failure.progress},
        };
        return toolResult(
            envelope(false, op, Json(nullptr), std::move(error), Json::array(), nextCallsFor(failure.code)),
            std::string{op} + " failed: [" + failure.code + "] " + failure.message,
            std::nullopt
        );
    }

    Json renderPayload(std::string_view op, Payload payload) {
        return toolResult(
            envelope(true, op, std::move(payload.data), Json(nullptr), std::move(payload.warnings), Json::array()),
            payload.summary,
            payload.image
        );
    }

    // --- 操作实现 ---

    struct Context {
        int pid = 0;
    };

    Json fieldsJson(std::span<const FieldSpec> fields) {
        Json listed = Json::array();
        for (const auto& spec : fields) {
            listed.push_back(
                Json{
                    {"name", spec.name},
                    {"required", spec.required},
                    {"hint", spec.hint},
                }
            );
        }
        return listed;
    }

    Json stepsHelp() {
        Json listed = Json::array();
        for (const auto& spec : StepSpecs) {
            listed.push_back(Json{{"do", spec.name}, {"summary", spec.summary}, {"fields", fieldsJson(spec.fields)}});
        }
        return Json{{"steps", std::move(listed)}};
    }

    Json keysHelp() {
        std::vector<std::string> names;
        for (const auto& key : Engine::keyTable()) {
            names.emplace_back(key.name);
        }
        return Json{
            {"keys",
             Json{
                 {"names", std::move(names)},
                 {"combos", "Join with '+' (\"ctrl+r\") or pass an array ([\"ctrl\", \"r\"])."},
                 {"note", "Names denote physical key positions (scan codes), which is what the game reads."},
             }},
        };
    }

    Json errorsHelp() {
        Json listed = Json::array();
        for (const auto& recovery : Recoveries) {
            listed.push_back(Json{{"code", recovery.code}, {"next", recovery.op}, {"reason", recovery.reason}});
        }
        return Json{{"errors", std::move(listed)}};
    }

    Json recipesHelp() {
        return Json{
            {"recipes",
             Json::array({
                 Json{
                     {"goal", "长按前进 2 秒"},
                     {"call",
                      Json{
                          {"op", "/run"},
                          {"args",
                           {{"steps",
                             Json::array(
                                 {Json{{"do", "key"}, {"keys", "w"}, {"action", "down"}},
                                  Json{{"do", "wait"}, {"ms", 2000}},
                                  Json{{"do", "key"}, {"keys", "w"}, {"action", "up"}}}
                             )}}}
                      }}
                 },
                 Json{
                     {"goal", "按住挖掘方块 1.5 秒"},
                     {
                         "call",
                         Json{{"op", "/click"}, {"args", {{"hold_ms", 1500}}}},
                     },
                     {"note", "游戏内不要传 at：指针被独占，坐标没有意义"}
                 },
                 Json{
                     {"goal", "背包里把物品从一格拖到另一格"},
                     {"call",
                      Json{
                          {"op", "/drag"},
                          {"args", {{"from", Json::array({0.31, 0.55})}, {"to", Json::array({0.62, 0.55})}}}
                      }}
                 },
                 Json{
                     {"goal", "跨调用保持按住，中途做别的事"},
                     {
                         "call",
                         Json{{"op", "/key"}, {"args", {{"keys", "shift"}, {"action", "down"}, {"leave_held", true}}}},
                     },
                     {"note", "结束后必须调用 /release-all，否则按键会一直按着"}
                 },
                 Json{
                     {"goal", "先校验坐标再真正执行"},
                     {"call", Json{{"op", "/click"}, {"args", {{"at", Json::array({0.5, 0.5})}, {"dry_run", true}}}}}
                 },
             })}
        };
    }

    Json overviewHelp() {
        return Json{
            {"operations",
             Json::array(
                 {Json{{"op", "/help"}, {"summary", "本说明；topic 可取 steps / options / keys / errors / recipes"}},
                  Json{
                      {"op", "/state"},
                      {"summary", "查询窗口尺寸、前台状态、指针是否被游戏独占、输入法状态、当前仍按住的键"}
                  },
                  Json{{"op", "/run"}, {"summary", "一次调用按顺序执行多个输入步骤"}},
                  Json{{"op", "/release-all"}, {"summary", "释放所有以 leave_held 保留按下的输入"}},
                  Json{
                      {"op", "/click 等单步糖"},
                      {"summary", "move / click / drag / scroll / look / key / text / wait，等价于只有一步的 /run"}
                  }}
             )},
            {"coords", "一律是客户区百分比 0.0-1.0，与 capture_game_window 截图同构；(0,0) 左上，(1,1) 右下"},
            {"topics", Json::array({"steps", "options", "keys", "errors", "recipes"})},
        };
    }

    Json optionsHelp() {
        Json options = fieldsJson(StepsField);
        for (const auto& option : fieldsJson(RunOptions)) {
            options.push_back(option);
        }
        return Json{{"options", std::move(options)}};
    }

    struct HelpTopic {
        std::string_view name;
        Json (*build)();
    };

    constexpr HelpTopic HelpTopics[]{
        {"overview", &overviewHelp},
        {"steps", &stepsHelp},
        {"options", &optionsHelp},
        {"keys", &keysHelp},
        {"errors", &errorsHelp},
        {"recipes", &recipesHelp},
    };

    std::string helpTopicNames() {
        std::string text;
        for (const auto& topic : HelpTopics) {
            text += text.empty() ? "" : ", ";
            text += topic.name;
        }
        return text;
    }

    OpResult opHelp(const Context&, const Json& args) {
        const auto        requested = args.find("topic");
        const std::string topic     = requested == args.end() || requested->get<std::string>().empty()
                                        ? "overview"
                                        : requested->get<std::string>();

        const auto found = std::ranges::find(HelpTopics, topic, &HelpTopic::name);
        if (found == std::end(HelpTopics)) {
            return invalid("topic: expected one of " + helpTopicNames() + ".");
        }

        Json data = Json{
            {"tool", mcdk::mc_input_mcp::ToolName},
            {"invocation", Json{{"op", "/..."}, {"args", Json::object()}}},
            {"semantics",
             "A successful call means the input was dispatched to the system queue, not that the game reacted. "
             "Verify with capture_game_window, logs, or capture=end."},
        };
        data.update(found->build());

        return Payload{std::move(data), Json::array(), "mc_input help (" + topic + ")", std::nullopt};
    }

    OpResult opState(const Context& context, const Json& args) {
        return validateFields(args, {}, "/state", {}).and_then([&]() -> OpResult {
            return Engine::inspect(context.pid)
                .transform([](const Engine::WindowInfo& window) {
                    Json data = Json{
                        {"window", windowJson(window)},
                        {"held", Engine::heldKeys()},
                    };
                    const std::string summary =
                        "Game window " + std::to_string(window.width) + "x" + std::to_string(window.height)
                        + (window.foreground ? ", foreground" : ", background")
                        + (window.pointerLocked ? ", pointer held by the game (use look / in-place click)"
                                                : ", pointer free (absolute coordinates apply)")
                        + (window.imeOpen ? ", IME is on (input batches suppress it by default)." : ".");
                    return Payload{std::move(data), Json::array(), summary, std::nullopt};
                })
                .transform_error(toFailure);
        });
    }

    OpResult opReleaseAll(const Context& context, const Json& args) {
        return validateFields(args, {}, "/release-all", {}).and_then([&]() -> OpResult {
            return Engine::releaseHeld(context.pid)
                .transform([](const Engine::Report& report) {
                    Json              data = reportJson(report);
                    const std::string summary =
                        report.released.empty()
                            ? "No held input remained; nothing to release."
                            : "Released " + std::to_string(report.released.size()) + " held input(s).";
                    return Payload{std::move(data), Json::array(), summary, std::nullopt};
                })
                .transform_error(toFailure);
        });
    }

    std::optional<std::string> captureIfRequested(const Context& context, bool requested) {
        if (!requested) return std::nullopt;
        const auto frame = MCDevTool::Style::captureMinecraftWindow480p(context.pid);
        return frame.has_value() && !frame->empty()
                 ? std::optional{base64::encode(reinterpret_cast<const char*>(frame->data()), frame->size())}
                 : std::nullopt;
    }

    Json buildWarnings(const Engine::Report& report, bool captureRequested, bool captureAvailable) {
        Json warnings = Json::array();
        if (!report.held.empty()) {
            warnings.push_back(
                Json{
                    {"code", "INPUT_STILL_HELD"},
                    {"message", "Input is still held down; call /release-all when the sequence is finished."},
                    {"held", report.held},
                }
            );
        }
        if (captureRequested && !captureAvailable) {
            warnings.push_back(
                Json{
                    {"code", "CAPTURE_UNAVAILABLE"},
                    {"message", "The follow-up screenshot could not be taken; the input itself was dispatched."},
                }
            );
        }
        return warnings;
    }

    OpResult opRun(const Context& context, const Json& args) {
        return validateFields(args, RunOptions, "/run", {"steps"})
            .and_then([&] { return readSteps(args); })
            .and_then([&](std::vector<Engine::Step> steps) -> OpResult {
                const auto options = readOptions(args);
                return Engine::run(context.pid, steps, options)
                    .transform([&](const Engine::Report& report) {
                        const bool wanted = wantsCapture(args) && !report.dryRun;
                        auto       image  = captureIfRequested(context, wanted);

                        const std::string summary =
                            report.dryRun
                                ? "Validated " + std::to_string(report.total)
                                      + " step(s) and resolved their coordinates; no input was sent."
                                : "Dispatched " + std::to_string(report.executed) + "/" + std::to_string(report.total)
                                      + " step(s) to the game window. Input is queued, not confirmed — verify with "
                                        "capture_game_window or logs.";

                        return Payload{
                            reportJson(report),
                            buildWarnings(report, wanted, image.has_value()),
                            summary,
                            std::move(image),
                        };
                    })
                    .transform_error(toFailure);
            });
    }

    // --- 操作表 ---

    struct OpSpec {
        std::string_view name;
        std::string_view step; // 非空表示这是一个单步糖
        OpResult (*handle)(const Context&, const Json&);
        std::string_view summary;
    };

    constexpr OpSpec Ops[]{
        {"/help", "", &opHelp, "用法说明"},
        {"/state", "", &opState, "窗口与指针状态"},
        {"/run", "", &opRun, "按顺序执行多个输入步骤"},
        {"/release-all", "", &opReleaseAll, "释放所有保持按下的输入"},
        {"/move", Engine::MoveStep::kName, nullptr, "单步糖"},
        {"/click", Engine::ClickStep::kName, nullptr, "单步糖"},
        {"/drag", Engine::DragStep::kName, nullptr, "单步糖"},
        {"/scroll", Engine::ScrollStep::kName, nullptr, "单步糖"},
        {"/look", Engine::LookStep::kName, nullptr, "单步糖"},
        {"/key", Engine::KeyStep::kName, nullptr, "单步糖"},
        {"/text", Engine::TextStep::kName, nullptr, "单步糖"},
        {"/wait", Engine::WaitStep::kName, nullptr, "单步糖"},
    };

    std::string opNames() {
        std::string text;
        for (const auto& spec : Ops) {
            text += text.empty() ? "" : ", ";
            text += spec.name;
        }
        return text;
    }

    // 单步糖：把参数按“属于步骤”还是“属于批次”拆开，再走同一条 /run 通路。
    OpResult runSugar(const Context& context, const OpSpec& spec, const Json& args) {
        const auto* stepSpec = findStepSpec(spec.step);
        if (!args.is_object()) {
            return invalid(std::string{spec.name} + ": expected an args object.");
        }

        Json step = Json{{"do", spec.step}};
        Json run  = Json::object();
        for (const auto& [key, value] : args.items()) {
            const bool belongsToStep =
                std::ranges::find(stepSpec->fields, std::string_view{key}, &FieldSpec::name) != stepSpec->fields.end();
            (belongsToStep ? step : run)[key] = value;
        }
        run["steps"] = Json::array({std::move(step)});
        return opRun(context, run);
    }

    // --- 分发 ---

    struct Request {
        std::string op;
        Json        args = Json::object();
    };

    Outcome<Request> parseEnvelope(const Json& arguments) {
        if (!arguments.is_object()) {
            return invalid("Arguments must be an object shaped like {op: \"/help\", args: {}}.");
        }
        for (const auto& [key, value] : arguments.items()) {
            if (key != "op" && key != "args") {
                return invalid("Unknown top-level field \"" + key + "\"; only op and args are accepted.");
            }
        }
        const auto op = arguments.find("op");
        if (op == arguments.end() || !op->is_string()) {
            return invalid("op is required and must be a string. Available: " + opNames() + ".");
        }
        const auto args = arguments.find("args");
        if (args != arguments.end() && !args->is_object()) {
            return invalid("args must be an object.");
        }
        return Request{op->get<std::string>(), args == arguments.end() ? Json::object() : *args};
    }

    OpResult dispatch(const Context& context, const Json& arguments) {
        return parseEnvelope(arguments).and_then([&](const Request& request) -> OpResult {
            const auto found = std::ranges::find(Ops, request.op, &OpSpec::name);
            if (found == std::end(Ops)) {
                return std::unexpected(
                    Failure{
                        "UNKNOWN_OPERATION",
                        "Unknown operation \"" + request.op + "\". Available: " + opNames() + ".",
                        false,
                        Json(nullptr),
                    }
                );
            }
            return found->step.empty() ? found->handle(context, request.args) : runSugar(context, *found, request.args);
        });
    }

    std::string opOf(const Json& arguments) {
        return arguments.is_object() && arguments.contains("op") && arguments["op"].is_string()
                 ? arguments["op"].get<std::string>()
                 : std::string{"?"};
    }

} // namespace

namespace mcdk::mc_input_mcp {

    nlohmann::json
    buildErrorResult(std::string_view op, std::string_view code, std::string_view message, bool retryable) {
        return renderFailure(op, Failure{std::string{code}, std::string{message}, retryable, Json(nullptr)});
    }

    // 全工具唯一的 try：任何未归类的异常都在这里变成 INTERNAL 错误信封。
    nlohmann::json handleRequest(int pid, const nlohmann::json& arguments) noexcept {
        try {
            const auto result = dispatch(Context{pid}, arguments);
            return result.has_value() ? renderPayload(opOf(arguments), std::move(*result))
                                      : renderFailure(opOf(arguments), result.error());
        } catch (const std::exception& error) {
            return buildErrorResult(opOf(arguments), "INTERNAL", error.what(), false);
        } catch (...) {
            return buildErrorResult(opOf(arguments), "INTERNAL", "Unknown internal failure.", false);
        }
    }

} // namespace mcdk::mc_input_mcp

namespace mcdk::mcp_tool_definitions {

    mcp::tool buildMcInputTool() {
        mcp::tool tool;
        tool.name = std::string(mc_input_mcp::ToolName);
        tool.description =
            "Drives the Minecraft game window through keyboard and mouse input. One call can run a whole ordered "
            "sequence: clicks, long presses, drags, wheel, camera motion, text and waits. Call /help first; /state "
            "reports window geometry and whether the game currently holds the pointer. Coordinates default to the "
            "0.0-1.0 percentage space shared with capture_game_window. A successful result means input was dispatched "
            "to the system queue, not that the game reacted - verify with capture_game_window or logs. Input uses "
            "{op:'/...', args:{...}}.";
        tool.parameters_schema = {
            {"type", "object"},
            {"required", Json::array({"op"})},
            {"properties",
             {{"op", {{"type", "string"}, {"description", "Operation such as /help, /state, /run, or /click."}}},
              {"args", {{"type", "object"}, {"description", "Strict operation-specific arguments."}}}}},
            {"additionalProperties", false},
        };
        tool.output_schema = {
            {"type", "object"},
            {"required", Json::array({"ok", "op", "data", "error", "warnings", "next_calls"})},
            {"properties",
             {{"ok", {{"type", "boolean"}}},
              {"op", {{"type", "string"}}},
              {"data", {{"type", Json::array({"object", "null"})}}},
              {"error", {{"type", Json::array({"object", "null"})}}},
              {"warnings", {{"type", "array"}, {"items", {{"type", "object"}}}}},
              {"next_calls", {{"type", "array"}, {"maxItems", 3}, {"items", {{"type", "object"}}}}}}},
            {"additionalProperties", false},
        };
        tool.annotations.read_only_hint   = false;
        tool.annotations.destructive_hint = false;
        tool.annotations.idempotent_hint  = false;
        tool.annotations.open_world_hint  = true;
        return tool;
    }

} // namespace mcdk::mcp_tool_definitions
