#include <mcdk/mc_input_mcp.hpp>
#include <mcdk/log_buffer.hpp>

#include <algorithm>
#include <cmath>
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

    constexpr std::size_t MaxSteps        = 64;
    constexpr int         MaxWaitMs       = 10000;
    constexpr int         MaxBudgetMs     = 120000;
    constexpr double      MaxRelativeMove = 100000.0; // 单步相对位移上限，防止换算成 int 时溢出

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
            // 范围在校验阶段就拒绝，否则要跑到该步骤才发现，前面的输入已经投递出去了。
            return validatePair(value, label).and_then([&]() -> Outcome<void> {
                const double x = value[0].get<double>();
                const double y = value[1].get<double>();
                return x >= 0.0 && x <= 1.0 && y >= 0.0 && y <= 1.0
                         ? Outcome<void>{}
                         : Outcome<void>{invalid(label + ": coordinates must be within 0.0-1.0 (client-area percentages).")};
            });
        case FieldType::PointRel:
            return validatePair(value, label).and_then([&]() -> Outcome<void> {
                const bool bounded = std::abs(value[0].get<double>()) <= MaxRelativeMove
                                  && std::abs(value[1].get<double>()) <= MaxRelativeMove;
                return bounded ? Outcome<void>{}
                               : Outcome<void>{invalid(
                                     label + ": relative movement must stay within ±"
                                     + std::to_string(static_cast<long long>(MaxRelativeMove)) + " counts per step."
                                 )};
            });
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

    constexpr FieldSpec SyncFields[]{
        {"at_ms",
         FieldType::Int,
         true,
         0,
         MaxBudgetMs,
         {},
         "等到批次开始后的第 N 毫秒再继续，已过则立即继续；与 wait 不同，不随执行开销漂移"},
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

    Engine::Step parseSync(const Json& step) { return Engine::SyncStep{intField(step, "at_ms", 0)}; }

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
        {Engine::SyncStep::kName, SyncFields, &parseSync, "等到批次起点后的绝对时刻，用于不漂移的精确编排"},
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
        {"restore_cursor", FieldType::Bool, false, 0, 0, {}, "结束后把鼠标放回原处，默认 false，保留操作结束时的位置"},
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
        {"logs",
         FieldType::Enum,
         false,
         0,
         0,
         CaptureNames,
         "end=结束后附带最近日志（包含历史日志），默认 none；dry_run 不附带"},
        {"logs_max_count", FieldType::Int, false, 1, 200, {}, "附带日志的最大条数，默认 20，按时间从旧到新排列"},
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
        options.restoreCursor      = boolField(args, "restore_cursor", options.restoreCursor);
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

    // --- 时间轴：以绝对时刻编排，按键与鼠标的按住可以相互重叠 ---
    //
    // 每个事件给出 at_ms（相对批次起点）；key / click 再给 hold_ms 表示按住多久。
    // 编译时把它们拆成「按下 @at_ms」与「松开 @at_ms + hold_ms」两个原语，全体按时刻稳定排序，
    // 时刻变化处插入 sync 步骤，再交给顺序引擎执行。move / drag / scroll / look / text 是原子步骤，
    // 占用自身的执行时间，落在这段时间内的后续事件会顺延到它们完成之后。

    constexpr FieldSpec TimelineAtField{
        "at_ms", FieldType::Int, true, 0, MaxBudgetMs, {}, "事件开始时刻，批次开始后的毫秒数"
    };

    constexpr FieldSpec TimelineKeyFields[]{
        {"keys", FieldType::Keys, true, 0, 0, {}, "按键或组合键，例如 \"w\" 或 \"w+d\"，整组同时按住"},
        {"hold_ms", FieldType::Int, false, 0, MaxBudgetMs, {}, "按住时长，默认 50；松开时刻 = at_ms + hold_ms"},
    };

    constexpr FieldSpec TimelineClickFields[]{
        {"at", FieldType::PointAbs, false, 0, 0, {}, "按下位置，客户区百分比 [x, y]；省略则在指针当前位置按下"},
        {"button", FieldType::Enum, false, 0, 0, ButtonNames, "鼠标键，默认 left"},
        {"hold_ms", FieldType::Int, false, 0, MaxBudgetMs, {}, "按住时长，默认 50；松开时刻 = at_ms + hold_ms"},
        {"modifiers", FieldType::Keys, false, 0, 0, {}, "整段按住期间同时按住的修饰键，例如 shift"},
    };

    struct TimelineSpec {
        std::string_view           name;
        std::span<const FieldSpec> fields;
        std::string_view           summary;
    };

    constexpr TimelineSpec TimelineSpecs[]{
        {Engine::KeyStep::kName, TimelineKeyFields, "在 at_ms 按下、at_ms + hold_ms 松开的按键或组合键"},
        {Engine::ClickStep::kName, TimelineClickFields, "在 at_ms 按下、at_ms + hold_ms 松开的鼠标键"},
        {Engine::MoveStep::kName, MoveFields, "在 at_ms 移动指针"},
        {Engine::DragStep::kName, DragFields, "在 at_ms 开始拖拽（原子步骤，占用自身执行时间）"},
        {Engine::ScrollStep::kName, ScrollFields, "在 at_ms 滚动滚轮（原子步骤）"},
        {Engine::LookStep::kName, LookFields, "在 at_ms 转动视角（原子步骤）"},
        {Engine::TextStep::kName, TextFields, "在 at_ms 输入文本（原子步骤）"},
    };

    std::string timelineEventNames() {
        std::string text;
        for (const auto& spec : TimelineSpecs) {
            text += text.empty() ? "" : ", ";
            text += spec.name;
        }
        return text;
    }

    const TimelineSpec* findTimelineSpec(std::string_view name) {
        const auto found = std::ranges::find(TimelineSpecs, name, &TimelineSpec::name);
        return found == std::end(TimelineSpecs) ? nullptr : &*found;
    }

    struct Scheduled {
        int          at = 0;
        Engine::Step step;
    };

    struct Timeline {
        std::vector<Engine::Step> steps; // 含 sync 同步点的顺序步骤
        std::size_t               events = 0;
        int                       endMs  = 0; // 最后一个原语的时刻
    };

    Outcome<void> validateTimelineEvent(const Json& event, std::size_t index) {
        const std::string where = "events[" + std::to_string(index) + "]";
        if (!event.is_object() || !event.contains("do") || !event["do"].is_string()) {
            return invalid(
                where + ": every event needs a string \"do\" field. Available: " + timelineEventNames() + "."
            );
        }

        const auto* spec = findTimelineSpec(event["do"].get<std::string>());
        if (spec == nullptr) {
            return invalid(
                where + ": unknown event \"" + event["do"].get<std::string>() + "\". Available: " + timelineEventNames()
                + ". A timeline has no wait event; place events with at_ms instead."
            );
        }
        return validateFields(event, spec->fields, where, {"do", "at_ms"}).and_then([&]() -> Outcome<void> {
            const auto at = event.find("at_ms");
            return at == event.end() ? Outcome<void>{invalid(where + ": missing required field \"at_ms\".")}
                                     : validateField(*at, TimelineAtField, where);
        });
    }

    void scheduleEvent(const Json& event, std::vector<Scheduled>& out) {
        const std::string kind = event["do"].get<std::string>();
        const int         at   = intField(event, "at_ms", 0);

        if (kind == Engine::KeyStep::kName) {
            const auto keys = readKeys(event.at("keys"));
            const int  hold = intField(event, "hold_ms", 50);
            out.push_back({at, Engine::KeyStep{keys, Engine::PressAction::Down, 0, 1}});
            out.push_back({at + hold, Engine::KeyStep{keys, Engine::PressAction::Up, 0, 1}});
            return;
        }
        if (kind == Engine::ClickStep::kName) {
            const auto modifiers = keysField(event, "modifiers");
            const auto button    = enumField(event, "button", ButtonNames, Engine::MouseButton::Left);
            const int  hold      = intField(event, "hold_ms", 50);
            const auto point     = event.find("at");

            // 修饰键要覆盖整段按住，因此拆成独立的按下 / 松开，而不是交给 click 步骤自己包住。
            for (const auto& key : modifiers) {
                out.push_back({at, Engine::KeyStep{{key}, Engine::PressAction::Down, 0, 1}});
            }
            Engine::ClickStep down;
            down.at     = point == event.end() ? std::optional<Engine::Point>{} : std::optional{pointField(*point)};
            down.button = button;
            down.action = Engine::PressAction::Down;
            out.push_back({at, down});

            Engine::ClickStep up;
            up.button = button;
            up.action = Engine::PressAction::Up;
            out.push_back({at + hold, up});
            for (const auto& key : modifiers | std::views::reverse) {
                out.push_back({at + hold, Engine::KeyStep{{key}, Engine::PressAction::Up, 0, 1}});
            }
            return;
        }
        out.push_back({at, findStepSpec(kind)->parse(event)}); // 原子步骤
    }

    Outcome<Timeline> compileTimeline(const Json& args, int budgetMs) {
        const auto found = args.find("events");
        if (found == args.end() || !found->is_array() || found->empty()) {
            return invalid("events: expected a non-empty array of event objects.");
        }
        if (found->size() > MaxSteps) {
            return invalid("events: at most " + std::to_string(MaxSteps) + " events per call.");
        }

        Outcome<void> checked{};
        for (std::size_t index = 0; index < found->size(); ++index) {
            checked = checked.and_then([&] { return validateTimelineEvent((*found)[index], index); });
        }

        return checked.and_then([&]() -> Outcome<Timeline> {
            std::vector<Scheduled> scheduled;
            for (const auto& event : *found) {
                scheduleEvent(event, scheduled);
            }
            std::ranges::stable_sort(scheduled, {}, &Scheduled::at);

            Timeline timeline;
            timeline.events = found->size();
            int lastAt      = -1;
            for (auto& item : scheduled) {
                if (item.at != lastAt) {
                    timeline.steps.push_back(Engine::SyncStep{item.at});
                    lastAt = item.at;
                }
                timeline.steps.push_back(std::move(item.step));
            }
            timeline.endMs = lastAt;

            if (timeline.endMs >= budgetMs) {
                return invalid(
                    "events: the timeline ends at " + std::to_string(timeline.endMs) + " ms, which exceeds budget_ms ("
                    + std::to_string(budgetMs) + "). Raise budget_ms or shorten the timeline."
                );
            }
            return timeline;
        });
    }

    // --- 步骤 -> JSON：让调用方看到编排结果 ---

    Json pointJson(Engine::Point point) { return Json::array({point.x, point.y}); }

    Json keyNamesJson(std::span<const Engine::Key> keys) {
        Json names = Json::array();
        for (const auto& key : keys) {
            names.push_back(std::string{key.name});
        }
        return names;
    }

    std::string_view actionName(Engine::PressAction action) {
        return ActionNames[static_cast<std::size_t>(action)];
    }

    std::string_view buttonName(Engine::MouseButton button) {
        return ButtonNames[static_cast<std::size_t>(button)];
    }

    struct StepJson {
        Json operator()(const Engine::MoveStep& step) const { return Json{{"do", "move"}, {"at", pointJson(step.at)}}; }

        Json operator()(const Engine::ClickStep& step) const {
            Json json{
                {"do", "click"},
                {"button", buttonName(step.button)},
                {"action", actionName(step.action)},
                {"hold_ms", step.holdMs},
            };
            if (step.at.has_value()) json["at"] = pointJson(*step.at);
            if (!step.modifiers.empty()) json["modifiers"] = keyNamesJson(step.modifiers);
            return json;
        }

        Json operator()(const Engine::DragStep& step) const {
            Json json{
                {"do", "drag"},
                {"from", pointJson(step.from)},
                {"to", pointJson(step.to)},
                {"button", buttonName(step.button)},
                {"hold_ms", step.holdMs},
                {"segments", step.segments},
            };
            if (!step.modifiers.empty()) json["modifiers"] = keyNamesJson(step.modifiers);
            return json;
        }

        Json operator()(const Engine::ScrollStep& step) const {
            Json json{
                {"do", "scroll"},
                {"amount", step.amount},
                {"axis", AxisNames[static_cast<std::size_t>(step.axis)]},
            };
            if (step.at.has_value()) json["at"] = pointJson(*step.at);
            if (!step.modifiers.empty()) json["modifiers"] = keyNamesJson(step.modifiers);
            return json;
        }

        Json operator()(const Engine::LookStep& step) const {
            return Json{{"do", "look"}, {"by", Json::array({step.dx, step.dy})}, {"segments", step.segments}};
        }

        Json operator()(const Engine::KeyStep& step) const {
            return Json{
                {"do", "key"},
                {"keys", keyNamesJson(step.keys)},
                {"action", actionName(step.action)},
                {"hold_ms", step.holdMs},
                {"repeat", step.repeat},
            };
        }

        Json operator()(const Engine::TextStep& step) const { return Json{{"do", "text"}, {"value", step.value}}; }

        Json operator()(const Engine::WaitStep& step) const { return Json{{"do", "wait"}, {"ms", step.ms}}; }

        Json operator()(const Engine::SyncStep& step) const { return Json{{"do", "sync"}, {"at_ms", step.atMs}}; }
    };

    Json planJson(std::span<const Engine::Step> steps) {
        Json plan = Json::array();
        for (const auto& step : steps) {
            plan.push_back(std::visit(StepJson{}, step));
        }
        return plan;
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
        const auto& data = body.at("ok").get<bool>() ? body.at("data") : body.at("error").at("progress");
        if (data.is_object() && data.contains("logs")) {
            const auto& logs = data.at("logs");
            std::string text =
                logs.at("available").get<bool>() ? "Recent game logs (oldest first):" : "Game log buffer unavailable.";
            for (const auto& line : logs.at("entries")) {
                text += "\n" + line.get<std::string>();
            }
            content.push_back(Json{{"type", "text"}, {"text", std::move(text)}});
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
        int              pid       = 0;
        mcdk::LogBuffer* logBuffer = nullptr;
    };

    void appendLogs(const Context& context, const Json& args, Json& data) {
        if (args.value("logs", "none") != "end" || boolField(args, "dry_run", false)) {
            return;
        }
        const auto maxCount = static_cast<std::size_t>(intField(args, "logs_max_count", 20));
        data["logs"]        = Json{
                   {"available", context.logBuffer != nullptr},
                   {"entries",
             context.logBuffer != nullptr ? context.logBuffer->getLatest(maxCount) : std::vector<std::string>{}},
                   {"order", "asc"},
                   {"max_count", maxCount},
        };
    }

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
                 Json{
                     {"goal", "按住 W 前进 2 秒，0.8 秒时跳一下，1.2 秒时按住左键 0.3 秒，1.5 秒时向右转视角"},
                     {"call",
                      Json{
                          {"op", "/timeline"},
                          {"args",
                           {{"events",
                             Json::array(
                                 {Json{{"at_ms", 0}, {"do", "key"}, {"keys", "w"}, {"hold_ms", 2000}},
                                  Json{{"at_ms", 800}, {"do", "key"}, {"keys", "space"}},
                                  Json{{"at_ms", 1200}, {"do", "click"}, {"hold_ms", 300}},
                                  Json{{"at_ms", 1500}, {"do", "look"}, {"by", Json::array({300, 0})}}}
                             )}}}
                      }},
                     {"note", "游戏内按住不同按键可以重叠；先加 dry_run 看 data.timeline.plan 确认编排"}
                 },
                 Json{
                     {"goal", "在聊天框执行指令：像真人一样按 T、打字、回车"},
                     {"call",
                      Json{
                          {"op", "/run"},
                          {"args",
                           {{"steps",
                             Json::array(
                                 {Json{{"do", "key"}, {"keys", "t"}},
                                  Json{{"do", "wait"}, {"ms", 300}},
                                  Json{{"do", "text"}, {"value", "/give @s diamond 1"}},
                                  Json{{"do", "key"}, {"keys", "enter"}}}
                             )}}}
                      }},
                     {"note", "走完整的客户端输入 → UI → 网络 → 服务端管线，Mod 监听到的聊天 / 指令事件与真人一致"}
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
                  Json{
                      {"op", "/timeline"},
                      {"summary",
                       "以绝对时刻编排一整段输入：每个事件给 at_ms（key / click 再给 hold_ms），按键与鼠标的按住可以"
                       "相互重叠，例如按住 W 前进的同时跳跃并点击；详见 topic=timeline"}
                  },
                  Json{{"op", "/release-all"}, {"summary", "释放所有以 leave_held 保留按下的输入"}},
                  Json{
                      {"op", "/click 等单步糖"},
                      {"summary", "move / click / drag / scroll / look / key / text / wait，等价于只有一步的 /run"}
                  }}
             )},
            {"coords", "一律是客户区百分比 0.0-1.0，与 capture_game_window 截图同构；(0,0) 左上，(1,1) 右下"},
            {"topics", Json::array({"steps", "timeline", "options", "keys", "errors", "recipes"})},
        };
    }

    Json timelineHelp() {
        Json listed = Json::array();
        for (const auto& spec : TimelineSpecs) {
            listed.push_back(Json{{"do", spec.name}, {"summary", spec.summary}, {"fields", fieldsJson(spec.fields)}});
        }
        return Json{
            {"timeline",
             Json{
                 {"invocation", Json{{"op", "/timeline"}, {"args", Json{{"events", Json::array()}}}}},
                 {"common_field", Json{{"name", "at_ms"}, {"required", true}, {"hint", TimelineAtField.hint}}},
                 {"events", std::move(listed)},
                 {"semantics",
                  Json::array(
                      {"key / click 在 at_ms 按下、at_ms + hold_ms 松开；不同事件的按住可以相互重叠，同一按键请勿重叠。",
                       "move / drag / scroll / look / text 是原子步骤，占用自身执行时间；落在这段时间内的事件顺延到其完成之后。",
                       "事件不必按时间排序；编译后按时刻稳定排序并插入 sync 同步点，时刻锚定批次起点，不随执行开销漂移。",
                       "整条时间轴必须在 budget_ms（默认 30000）内结束；结束时释放它按下的一切，不支持 leave_held。",
                       "dry_run 时在 data.timeline.plan 返回编译出的步骤序列，可先检查编排再真正执行。"}
                  )},
                 {"options", "与 /run 相同（focus / ime / budget_ms / capture / logs 等），但不接受 step_delay_ms 与 leave_held。"},
             }},
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
        {"timeline", &timelineHelp},
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

                        Json data = reportJson(report);
                        appendLogs(context, args, data);
                        return Payload{
                            std::move(data),
                            buildWarnings(report, wanted, image.has_value()),
                            summary,
                            std::move(image),
                        };
                    })
                    .transform_error([&](const Engine::Error& error) {
                        auto failure = toFailure(error);
                        appendLogs(context, args, failure.progress);
                        return failure;
                    });
            });
    }

    OpResult opTimeline(const Context& context, const Json& args) {
        return validateFields(args, RunOptions, "/timeline", {"events"})
            .and_then([&]() -> Outcome<void> {
                // 时间轴自己安排每一段间隔并释放它按下的一切，这两项批次选项在这里没有意义。
                for (const std::string_view name : {"step_delay_ms", "leave_held"}) {
                    if (args.contains(name)) {
                        return invalid(
                            "/timeline: " + std::string{name}
                            + " is not supported; the timeline schedules every gap itself and releases everything it "
                              "pressed."
                        );
                    }
                }
                return {};
            })
            .and_then([&] { return compileTimeline(args, intField(args, "budget_ms", 30000)); })
            .and_then([&](Timeline timeline) -> OpResult {
                auto options        = readOptions(args);
                options.stepDelayMs = 0; // 间隔全部由 sync 表达

                return Engine::run(context.pid, timeline.steps, options)
                    .transform([&](const Engine::Report& report) {
                        const bool wanted = wantsCapture(args) && !report.dryRun;
                        auto       image  = captureIfRequested(context, wanted);

                        Json data        = reportJson(report);
                        data["timeline"] = Json{
                            {"events", timeline.events},
                            {"steps", timeline.steps.size()},
                            {"end_ms", timeline.endMs},
                        };
                        if (report.dryRun) {
                            data["timeline"]["plan"] = planJson(timeline.steps);
                        }
                        appendLogs(context, args, data);

                        const std::string shape = std::to_string(timeline.events) + "-event timeline ("
                                                + std::to_string(timeline.steps.size()) + " steps, ends at "
                                                + std::to_string(timeline.endMs) + " ms)";
                        const std::string summary =
                            report.dryRun
                                ? "Validated a " + shape + " and resolved its coordinates; no input was sent."
                                : "Played a " + shape
                                      + " on the game window. Input is queued, not confirmed — verify with "
                                        "capture_game_window or logs.";
                        return Payload{
                            std::move(data),
                            buildWarnings(report, wanted, image.has_value()),
                            summary,
                            std::move(image),
                        };
                    })
                    .transform_error([&](const Engine::Error& error) {
                        auto failure = toFailure(error);
                        appendLogs(context, args, failure.progress);
                        return failure;
                    });
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
        {"/timeline", "", &opTimeline, "按绝对时间轴编排输入，按住可以相互重叠"},
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

    nlohmann::json compileTimelinePlan(const nlohmann::json& args) noexcept {
        try {
            const int  budget   = args.is_object() && args.contains("budget_ms") && args["budget_ms"].is_number()
                                    ? args["budget_ms"].get<int>()
                                    : 30000;
            const auto compiled = compileTimeline(args, budget);
            if (!compiled.has_value()) {
                return Json{
                    {"ok", false},
                    {"error", Json{{"code", compiled.error().code}, {"message", compiled.error().message}}},
                };
            }
            return Json{
                {"ok", true},
                {"events", compiled->events},
                {"end_ms", compiled->endMs},
                {"steps", planJson(compiled->steps)},
            };
        } catch (const std::exception& error) {
            return Json{{"ok", false}, {"error", Json{{"code", "INTERNAL"}, {"message", error.what()}}}};
        } catch (...) {
            return Json{{"ok", false}, {"error", Json{{"code", "INTERNAL"}, {"message", "Unknown internal failure."}}}};
        }
    }

    // 全工具唯一的 try：任何未归类的异常都在这里变成 INTERNAL 错误信封。
    nlohmann::json handleRequest(int pid, const nlohmann::json& arguments, LogBuffer* logBuffer) noexcept {
        try {
            const auto result = dispatch(Context{pid, logBuffer}, arguments);
            return result.has_value() ? renderPayload(opOf(arguments), std::move(*result))
                                      : renderFailure(opOf(arguments), result.error());
        } catch (const std::exception& error) {
            return buildErrorResult(opOf(arguments), "INTERNAL", error.what(), false);
        } catch (...) {
            return buildErrorResult(opOf(arguments), "INTERNAL", "Unknown internal failure.", false);
        }
    }

} // namespace mcdk::mc_input_mcp
