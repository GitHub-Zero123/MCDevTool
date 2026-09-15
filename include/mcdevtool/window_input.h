#pragma once
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace MCDevTool::Input {
    // 输入注入的全部失败原因。每一项都会原样映射成 MCP 侧的错误码，
    // 调用方据此决定是重试、换策略还是放弃，而不是面对一个笼统的 false。
    enum class ErrorCode : std::uint8_t {
        InvalidArgument,     // 参数本身不合法（坐标越界、步骤为空等）
        WindowNotFound,      // pid 未设置或窗口不存在
        WindowMinimized,     // 窗口最小化且不允许还原
        FocusDenied,         // 无法把目标窗口拉到前台
        FocusLost,           // 执行过程中前台被其它窗口抢走
        DesktopUnavailable,  // 锁屏、RDP 断开或无交互桌面
        PrivilegeBlocked,    // UIPI：目标进程完整性级别高于本进程
        PointerModeMismatch, // 指针被游戏独占时使用了绝对坐标
        Busy,                // 另一批输入正在执行
        DeadlineExceeded,    // 超出本批次时间预算
        GameExited,          // 执行过程中游戏窗口消失
        PartialFailure,      // SendInput 未能投递全部事件
        Unsupported,         // 当前平台不支持输入注入
        Internal,            // 兜底：未归类的内部错误
    };

    [[nodiscard]] std::string_view errorCodeName(ErrorCode code) noexcept;

    enum class MouseButton : std::uint8_t { Left, Right, Middle };
    enum class PressAction : std::uint8_t { Press, Down, Up, Double };
    enum class ScrollAxis : std::uint8_t { Vertical, Horizontal };
    enum class FocusPolicy : std::uint8_t { Auto, Require, Keep };

    // 游戏里的 WASD 是动作输入而不是打字：输入法开着时按键会被 IME 吞掉或转成候选词。
    // Suppress 在批次期间把目标窗口切到英数直通模式，结束后恢复原状。
    enum class ImePolicy : std::uint8_t { Suppress, Keep };

    // 坐标一律是客户区百分比（0.0-1.0），与 capture_game_window 的截图同构。
    // 不提供像素坐标：窗口尺寸一变，像素值就失去意义。
    struct Point {
        double x = 0.0;
        double y = 0.0;
    };

    struct Pixel {
        int x = 0;
        int y = 0;
    };

    // 物理按键位置（Set 1 扫描码）。游戏读取的是扫描码而非虚拟键码，
    // 因此这里不保存 VK，避免非 QWERTY 布局下按键落到错误的物理位置。
    struct Key {
        std::string_view name;
        std::uint16_t    scan     = 0;
        bool             extended = false;
    };

    struct MoveStep {
        Point at;

        static constexpr std::string_view kName = "move";
        [[nodiscard]] constexpr bool      usesAbsoluteCoords() const noexcept { return true; }
    };

    struct ClickStep {
        std::optional<Point> at; // 省略表示在当前指针位置按下，用于游戏内视角锁定状态
        MouseButton          button = MouseButton::Left;
        PressAction          action = PressAction::Press;
        int                  holdMs = 50;
        std::vector<Key>     modifiers;

        static constexpr std::string_view kName = "click";
        [[nodiscard]] constexpr bool      usesAbsoluteCoords() const noexcept { return at.has_value(); }
    };

    struct DragStep {
        Point            from;
        Point            to;
        MouseButton      button   = MouseButton::Left;
        int              holdMs   = 50;
        int              segments = 8;
        std::vector<Key> modifiers;

        static constexpr std::string_view kName = "drag";
        [[nodiscard]] constexpr bool      usesAbsoluteCoords() const noexcept { return true; }
    };

    struct ScrollStep {
        std::optional<Point> at;
        int                  amount = 0;
        ScrollAxis           axis   = ScrollAxis::Vertical;
        std::vector<Key>     modifiers;

        static constexpr std::string_view kName = "scroll";
        [[nodiscard]] constexpr bool      usesAbsoluteCoords() const noexcept { return at.has_value(); }
    };

    // 相对位移，单位是鼠标原始计数而非像素或角度：游戏灵敏度决定实际转动幅度。
    struct LookStep {
        double dx       = 0.0;
        double dy       = 0.0;
        int    segments = 4;

        static constexpr std::string_view kName = "look";
        [[nodiscard]] constexpr bool      usesAbsoluteCoords() const noexcept { return false; }
    };

    struct KeyStep {
        std::vector<Key> keys;
        PressAction      action = PressAction::Press;
        int              holdMs = 50;
        int              repeat = 1;

        static constexpr std::string_view kName = "key";
        [[nodiscard]] constexpr bool      usesAbsoluteCoords() const noexcept { return false; }
    };

    struct TextStep {
        std::string value; // UTF-8

        static constexpr std::string_view kName = "text";
        [[nodiscard]] constexpr bool      usesAbsoluteCoords() const noexcept { return false; }
    };

    struct WaitStep {
        int ms = 0;

        static constexpr std::string_view kName = "wait";
        [[nodiscard]] constexpr bool      usesAbsoluteCoords() const noexcept { return false; }
    };

    using Step = std::variant<MoveStep, ClickStep, DragStep, ScrollStep, LookStep, KeyStep, TextStep, WaitStep>;

    [[nodiscard]] std::string_view stepName(const Step& step) noexcept;

    // 绝对坐标步骤在指针被游戏独占时无意义，据此提前判定 PointerModeMismatch。
    [[nodiscard]] bool stepUsesAbsoluteCoords(const Step& step) noexcept;

    struct Options {
        FocusPolicy focus              = FocusPolicy::Auto;
        ImePolicy   ime                = ImePolicy::Suppress;
        int         stepDelayMs        = 60;
        int         budgetMs           = 30000;
        bool        restoreCursor      = false;
        bool        restoreIfMinimized = true;
        bool        leaveHeld          = false;
        bool        dryRun             = false;
    };

    struct StepOutcome {
        std::size_t          index = 0;
        std::string_view     kind;
        std::optional<Pixel> at; // 解析后的客户区像素，dry_run 下同样填充
        int                  elapsedMs = 0;
    };

    struct WindowInfo {
        int  width         = 0;
        int  height        = 0;
        bool foreground    = false;
        bool pointerLocked = false; // 游戏独占指针（光标隐藏）时为真
        bool imeOpen       = false; // 目标窗口当前是否处于输入法开启状态
    };

    // 无论成功还是失败都描述副作用停在哪里：执行到第几步、哪些键被释放、哪些仍按着。
    struct Report {
        std::size_t              total    = 0;
        std::size_t              executed = 0;
        bool                     dryRun   = false;
        WindowInfo               window;
        std::vector<StepOutcome> steps;
        std::vector<std::string> released;
        std::vector<std::string> held;
    };

    struct Error {
        ErrorCode   code = ErrorCode::Internal;
        std::string message;
        Report      progress;
    };

    template <typename T>
    using Result = std::expected<T, Error>;

    // 按顺序投递一批输入。返回成功只代表事件已进入系统队列，不代表游戏已经响应。
    [[nodiscard]] Result<Report> run(int pid, std::span<const Step> steps, const Options& options);

    // 查询窗口几何与指针状态，不产生任何输入。
    [[nodiscard]] Result<WindowInfo> inspect(int pid);

    // 释放此前以 leave_held 保留按下的所有按键与鼠标键。
    [[nodiscard]] Result<Report> releaseHeld(int pid);

    // 当前仍以 leave_held 保持按下的输入名字。
    [[nodiscard]] std::vector<std::string> heldKeys();

    // 按名字查找物理按键，名字来源于内置扫描码表。
    [[nodiscard]] std::optional<Key> findKey(std::string_view name) noexcept;

    // 全部受支持的按键名，用于 /help 输出与参数校验提示。
    [[nodiscard]] std::span<const Key> keyTable() noexcept;
} // namespace MCDevTool::Input
