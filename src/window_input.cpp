#include "mcdevtool/window_input.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <mutex>
#include <ranges>
#include <utility>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <imm.h>
#include "window_lookup.h"
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "imm32.lib")
#endif

namespace MCDevTool::Input {
    namespace {
        // 错误码 -> 对外名字。渲染层只查这张表，不做分支。
        struct ErrorName {
            ErrorCode        code;
            std::string_view name;
        };

        constexpr ErrorName ErrorNames[]{
            {ErrorCode::InvalidArgument, "INVALID_ARGUMENT"},
            {ErrorCode::WindowNotFound, "WINDOW_NOT_FOUND"},
            {ErrorCode::WindowMinimized, "WINDOW_MINIMIZED"},
            {ErrorCode::FocusDenied, "FOCUS_DENIED"},
            {ErrorCode::FocusLost, "FOCUS_LOST"},
            {ErrorCode::DesktopUnavailable, "DESKTOP_UNAVAILABLE"},
            {ErrorCode::PrivilegeBlocked, "PRIVILEGE_BLOCKED"},
            {ErrorCode::PointerModeMismatch, "POINTER_MODE_MISMATCH"},
            {ErrorCode::Busy, "BUSY"},
            {ErrorCode::DeadlineExceeded, "DEADLINE_EXCEEDED"},
            {ErrorCode::GameExited, "GAME_EXITED"},
            {ErrorCode::PartialFailure, "PARTIAL_FAILURE"},
            {ErrorCode::Unsupported, "UNSUPPORTED"},
            {ErrorCode::Internal, "INTERNAL"},
        };

        // 物理按键位置（Set 1 扫描码）。0xE0 前缀键统一标记为 extended。
        constexpr Key Keys[]{
            {"esc", 0x01},
            {"1", 0x02},
            {"2", 0x03},
            {"3", 0x04},
            {"4", 0x05},
            {"5", 0x06},
            {"6", 0x07},
            {"7", 0x08},
            {"8", 0x09},
            {"9", 0x0A},
            {"0", 0x0B},
            {"minus", 0x0C},
            {"equal", 0x0D},
            {"backspace", 0x0E},
            {"tab", 0x0F},
            {"q", 0x10},
            {"w", 0x11},
            {"e", 0x12},
            {"r", 0x13},
            {"t", 0x14},
            {"y", 0x15},
            {"u", 0x16},
            {"i", 0x17},
            {"o", 0x18},
            {"p", 0x19},
            {"bracketleft", 0x1A},
            {"bracketright", 0x1B},
            {"enter", 0x1C},
            {"ctrl", 0x1D},
            {"a", 0x1E},
            {"s", 0x1F},
            {"d", 0x20},
            {"f", 0x21},
            {"g", 0x22},
            {"h", 0x23},
            {"j", 0x24},
            {"k", 0x25},
            {"l", 0x26},
            {"semicolon", 0x27},
            {"apostrophe", 0x28},
            {"grave", 0x29},
            {"shift", 0x2A},
            {"backslash", 0x2B},
            {"z", 0x2C},
            {"x", 0x2D},
            {"c", 0x2E},
            {"v", 0x2F},
            {"b", 0x30},
            {"n", 0x31},
            {"m", 0x32},
            {"comma", 0x33},
            {"period", 0x34},
            {"slash", 0x35},
            {"rshift", 0x36},
            {"alt", 0x38},
            {"space", 0x39},
            {"capslock", 0x3A},
            {"f1", 0x3B},
            {"f2", 0x3C},
            {"f3", 0x3D},
            {"f4", 0x3E},
            {"f5", 0x3F},
            {"f6", 0x40},
            {"f7", 0x41},
            {"f8", 0x42},
            {"f9", 0x43},
            {"f10", 0x44},
            {"numlock", 0x45},
            {"scrolllock", 0x46},
            {"numpad7", 0x47},
            {"numpad8", 0x48},
            {"numpad9", 0x49},
            {"numpadminus", 0x4A},
            {"numpad4", 0x4B},
            {"numpad5", 0x4C},
            {"numpad6", 0x4D},
            {"numpadplus", 0x4E},
            {"numpad1", 0x4F},
            {"numpad2", 0x50},
            {"numpad3", 0x51},
            {"numpad0", 0x52},
            {"numpaddot", 0x53},
            {"f11", 0x57},
            {"f12", 0x58},
            {"rctrl", 0x1D, true},
            {"ralt", 0x38, true},
            {"numpadenter", 0x1C, true},
            {"numpadslash", 0x35, true},
            {"home", 0x47, true},
            {"up", 0x48, true},
            {"pageup", 0x49, true},
            {"left", 0x4B, true},
            {"right", 0x4D, true},
            {"end", 0x4F, true},
            {"down", 0x50, true},
            {"pagedown", 0x51, true},
            {"insert", 0x52, true},
            {"delete", 0x53, true},
        };
    } // namespace

    std::string_view errorCodeName(ErrorCode code) noexcept {
        const auto found = std::ranges::find(ErrorNames, code, &ErrorName::code);
        return found == std::end(ErrorNames) ? std::string_view{"INTERNAL"} : found->name;
    }

    std::optional<Key> findKey(std::string_view name) noexcept {
        const auto found = std::ranges::find(Keys, name, &Key::name);
        return found == std::end(Keys) ? std::nullopt : std::optional<Key>{*found};
    }

    std::span<const Key> keyTable() noexcept { return Keys; }

    std::string_view stepName(const Step& step) noexcept {
        return std::visit([](const auto& value) { return std::remove_cvref_t<decltype(value)>::kName; }, step);
    }

    bool stepUsesAbsoluteCoords(const Step& step) noexcept {
        return std::visit([](const auto& value) { return value.usesAbsoluteCoords(); }, step);
    }

    namespace {
#ifdef _WIN32
        using Clock = std::chrono::steady_clock;

        constexpr int FocusWaitMs   = 400;   // 等待窗口真正到达前台
        constexpr int FocusSettleMs = 120;   // 焦点切换后游戏重新接管输入所需的缓冲
        constexpr int RestoreWaitMs = 120;   // 还原最小化窗口后的缓冲
        constexpr int SliceMs       = 20;    // 睡眠切片，保证守卫及时生效
        constexpr int LockWaitMs    = 250;   // 输入互斥的等待上限
        constexpr int SegmentGapMs  = 8;     // 拖拽/视角插值点之间的间隔
        constexpr int DoubleGapMs   = 40;    // 双击两次按下之间的间隔
        constexpr int ImeWaitMs     = 200;   // 与目标进程 IME 窗口通信的等待上限
        constexpr int HeldLeaseMs   = 60000; // leave_held 的租期上限，超时视为泄漏

        // WM_IME_CONTROL 的子命令，公开的 imm.h 并未导出这几个常量。
        constexpr WPARAM ImcGetConversionMode = 0x0001;
        constexpr WPARAM ImcSetConversionMode = 0x0002;
        constexpr WPARAM ImcGetOpenStatus     = 0x0005;
        constexpr WPARAM ImcSetOpenStatus     = 0x0006;

        std::string describeForeground(); // 定义在前台策略一节

        std::unexpected<Error> fail(ErrorCode code, std::string message) {
            return std::unexpected(Error{code, std::move(message), {}});
        }

        // 依次执行若干动作，任意一步失败即短路并保留首个错误。
        template <typename... Actions>
        Result<void> sequence(Actions&&... actions) {
            Result<void> result{};
            ((result = result.and_then(std::forward<Actions>(actions))), ...);
            return result;
        }

        Result<void> repeatTimes(int count, auto&& action) {
            Result<void> result{};
            for (int index = 0; index < count && result.has_value(); ++index) {
                result = action();
            }
            return result;
        }

        // --- RAII 守卫：所有清理都挂在析构上，成功路径与失败路径共用同一份代码 ---

        struct ProcessHandle {
            HANDLE value = nullptr;

            explicit ProcessHandle(HANDLE handle) : value(handle) {}
            ProcessHandle(const ProcessHandle&)            = delete;
            ProcessHandle& operator=(const ProcessHandle&) = delete;

            ~ProcessHandle() {
                if (value != nullptr && value != INVALID_HANDLE_VALUE) CloseHandle(value);
            }

            explicit operator bool() const noexcept { return value != nullptr; }
        };

        struct DpiScope {
            DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

            DpiScope()                           = default;
            DpiScope(const DpiScope&)            = delete;
            DpiScope& operator=(const DpiScope&) = delete;

            ~DpiScope() {
                if (previous != nullptr) SetThreadDpiAwarenessContext(previous);
            }
        };

        // SetForegroundWindow 的权限判定看的是「调用线程」的状态，所以要把我们自己的线程
        // 挂到当前前台线程的输入队列上，借到它的前台权限。把两个外部线程互相挂接不起作用。
        struct ForegroundAttach {
            DWORD ours     = GetCurrentThreadId();
            DWORD foreign  = 0;
            bool  attached = false;

            explicit ForegroundAttach(HWND foregroundWindow)
            : foreign(GetWindowThreadProcessId(foregroundWindow, nullptr)) {
                attached = foreign != 0 && foreign != ours && AttachThreadInput(ours, foreign, TRUE) != FALSE;
            }

            ForegroundAttach(const ForegroundAttach&)            = delete;
            ForegroundAttach& operator=(const ForegroundAttach&) = delete;

            ~ForegroundAttach() {
                if (attached) AttachThreadInput(ours, foreign, FALSE);
            }
        };

        std::optional<LPARAM> imeControl(HWND ime, WPARAM command, LPARAM value) noexcept {
            DWORD_PTR  answer = 0;
            const auto sent   = SendMessageTimeoutW(
                ime,
                WM_IME_CONTROL,
                command,
                value,
                SMTO_ABORTIFHUNG | SMTO_BLOCK,
                ImeWaitMs,
                &answer
            );
            return sent == 0 ? std::nullopt : std::optional<LPARAM>{static_cast<LPARAM>(answer)};
        }

        bool imeIsOpen(HWND target) noexcept {
            HWND ime = ImmGetDefaultIMEWnd(target);
            return ime != nullptr && imeControl(ime, ImcGetOpenStatus, 0).value_or(0) != 0;
        }

        // 批次期间关闭输入法并切到英数，析构时按原样还回去。
        struct ImeScope {
            HWND   ime        = nullptr;
            LPARAM openStatus = 0;
            LPARAM conversion = 0;
            bool   armed      = false;

            ImeScope(HWND target, bool enabled) {
                ime = enabled ? ImmGetDefaultIMEWnd(target) : nullptr;
                if (ime == nullptr) return;

                const auto status = imeControl(ime, ImcGetOpenStatus, 0);
                const auto mode   = imeControl(ime, ImcGetConversionMode, 0);
                if (!status.has_value() || !mode.has_value()) return;

                openStatus = *status;
                conversion = *mode;
                armed      = true;
                imeControl(ime, ImcSetConversionMode, IME_CMODE_ALPHANUMERIC);
                imeControl(ime, ImcSetOpenStatus, 0);
            }

            ImeScope(const ImeScope&)            = delete;
            ImeScope& operator=(const ImeScope&) = delete;

            ~ImeScope() {
                if (!armed) return;
                imeControl(ime, ImcSetConversionMode, conversion);
                imeControl(ime, ImcSetOpenStatus, openStatus);
            }
        };

        struct CursorRestore {
            POINT origin{};
            bool  armed = false;

            explicit CursorRestore(bool enabled) : armed(enabled && GetCursorPos(&origin) != FALSE) {}

            CursorRestore(const CursorRestore&)            = delete;
            CursorRestore& operator=(const CursorRestore&) = delete;

            ~CursorRestore() {
                if (armed) SetCursorPos(origin.x, origin.y);
            }
        };

        // --- 输入事件构造 ---

        INPUT keyEvent(std::uint16_t scan, bool extended, bool up) noexcept {
            INPUT input{};
            input.type     = INPUT_KEYBOARD;
            input.ki.wScan = scan;
            input.ki.dwFlags =
                KEYEVENTF_SCANCODE | (extended ? KEYEVENTF_EXTENDEDKEY : 0u) | (up ? KEYEVENTF_KEYUP : 0u);
            return input;
        }

        INPUT unicodeEvent(wchar_t unit, bool up) noexcept {
            INPUT input{};
            input.type       = INPUT_KEYBOARD;
            input.ki.wScan   = static_cast<WORD>(unit);
            input.ki.dwFlags = KEYEVENTF_UNICODE | (up ? KEYEVENTF_KEYUP : 0u);
            return input;
        }

        DWORD buttonFlag(MouseButton button, bool up) noexcept {
            constexpr DWORD downFlags[]{MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_MIDDLEDOWN};
            constexpr DWORD upFlags[]{MOUSEEVENTF_LEFTUP, MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEUP};
            const auto      index = static_cast<std::size_t>(button);
            return up ? upFlags[index] : downFlags[index];
        }

        INPUT buttonEvent(MouseButton button, bool up) noexcept {
            INPUT input{};
            input.type       = INPUT_MOUSE;
            input.mi.dwFlags = buttonFlag(button, up);
            return input;
        }

        INPUT wheelEvent(int amount, ScrollAxis axis) noexcept {
            INPUT input{};
            input.type         = INPUT_MOUSE;
            input.mi.mouseData = static_cast<DWORD>(amount * WHEEL_DELTA);
            input.mi.dwFlags   = axis == ScrollAxis::Horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
            return input;
        }

        INPUT relativeMoveEvent(int dx, int dy) noexcept {
            INPUT input{};
            input.type       = INPUT_MOUSE;
            input.mi.dx      = dx;
            input.mi.dy      = dy;
            input.mi.dwFlags = MOUSEEVENTF_MOVE;
            return input;
        }

        // 归一化到整个虚拟桌面，副屏上的窗口才能得到正确坐标。
        std::optional<INPUT> absoluteMoveEvent(POINT screen) noexcept {
            const int left   = GetSystemMetrics(SM_XVIRTUALSCREEN);
            const int top    = GetSystemMetrics(SM_YVIRTUALSCREEN);
            const int width  = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            if (width <= 1 || height <= 1) return std::nullopt;

            INPUT input{};
            input.type       = INPUT_MOUSE;
            input.mi.dx      = static_cast<LONG>((screen.x - left) * 65535LL / (width - 1));
            input.mi.dy      = static_cast<LONG>((screen.y - top) * 65535LL / (height - 1));
            input.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE;
            return input;
        }

        bool emit(std::span<INPUT> events) noexcept {
            return SendInput(static_cast<UINT>(events.size()), events.data(), sizeof(INPUT)) == events.size();
        }

        // --- 已按下未释放的输入 ---

        struct Held {
            std::string       name;
            Clock::time_point since    = Clock::now();
            std::uint16_t     scan     = 0;
            bool              extended = false;
            bool              isButton = false;
            MouseButton       button   = MouseButton::Left;
        };

        Held asHeld(const Key& key) {
            return Held{std::string{key.name}, Clock::now(), key.scan, key.extended, false, MouseButton::Left};
        }

        Held asHeld(MouseButton button) {
            constexpr std::string_view names[]{"mouse:left", "mouse:right", "mouse:middle"};
            return Held{std::string{names[static_cast<std::size_t>(button)]}, Clock::now(), 0, false, true, button};
        }

        INPUT heldEvent(const Held& held, bool up) noexcept {
            return held.isButton ? buttonEvent(held.button, up) : keyEvent(held.scan, held.extended, up);
        }

        // 跨调用保留的按下状态，仅在持有输入互斥时访问。
        std::vector<std::string> releaseEntries(std::vector<Held>& entries) noexcept;

        // 跨调用保留的按下状态。析构挂在静态对象上：进程正常退出时无论走哪条路径，
        // 都不会把按键留在系统里——这比依赖调用方记得 /release-all 可靠得多。
        struct Registry {
            std::vector<Held> entries;

            Registry()                           = default;
            Registry(const Registry&)            = delete;
            Registry& operator=(const Registry&) = delete;

            ~Registry() { releaseEntries(entries); }
        };

        std::vector<Held>& registry() noexcept {
            static Registry held;
            return held.entries;
        }

        bool sameInput(const Held& left, const Held& right) noexcept {
            return left.isButton == right.isButton && left.button == right.button && left.scan == right.scan
                && left.extended == right.extended;
        }

        void eraseMatch(std::vector<Held>& entries, const Held& held) {
            auto       reversed = entries | std::views::reverse;
            const auto found =
                std::ranges::find_if(reversed, [&](const Held& entry) { return sameInput(entry, held); });
            if (found != reversed.end()) entries.erase(std::prev(found.base()));
        }

        std::vector<std::string> namesOf(const std::vector<Held>& entries) {
            std::vector<std::string> names;
            names.reserve(entries.size());
            for (const auto& entry : entries) names.push_back(entry.name);
            return names;
        }

        std::vector<std::string> releaseEntries(std::vector<Held>& entries) noexcept {
            std::vector<std::string> released;
            for (auto& entry : entries | std::views::reverse) {
                INPUT event = heldEvent(entry, true);
                emit(std::span{&event, 1});
                released.push_back(std::move(entry.name));
            }
            entries.clear();
            std::ranges::reverse(released);
            return released;
        }

        BOOL WINAPI onConsoleExit(DWORD) {
            releaseEntries(registry());
            return FALSE; // 继续走默认处理
        }

        // Ctrl+C 与关闭控制台不会执行静态析构，而这正是最容易把按键留在系统里的退出方式。
        // 只在真的有输入被跨调用保留时才安装，避免给库引入无谓的全局副作用。
        void installExitGuardOnce() {
            static const bool installed = SetConsoleCtrlHandler(&onConsoleExit, TRUE) != FALSE;
            (void)installed;
        }

        // 超过租期仍未释放的保留输入按泄漏处理：卡住的 shift 会让用户此后所有输入都是错的，
        // 代价远高于误放一个本该继续按住的键。
        std::vector<std::string> sweepExpired() {
            std::vector<Held> expired;
            auto&             kept = registry();
            for (auto it = kept.begin(); it != kept.end();) {
                const bool stale = Clock::now() - it->since >= std::chrono::milliseconds(HeldLeaseMs);
                if (stale) {
                    expired.push_back(std::move(*it));
                    it = kept.erase(it);
                } else {
                    ++it;
                }
            }
            return releaseEntries(expired);
        }

        // 批次内按下的输入。任何退出路径（正常、报错、异常）都由析构逆序释放。
        struct Ledger {
            std::vector<Held> entries;

            Ledger()                         = default;
            Ledger(const Ledger&)            = delete;
            Ledger& operator=(const Ledger&) = delete;

            ~Ledger() { releaseEntries(entries); }

            void add(Held held) { entries.push_back(std::move(held)); }

            // 释放必须同时销两本账：本批次的账本，以及跨调用保留的全局登记表。
            // 只销前者会让 action=up 释放掉的键永远留在 held 里，登记表只增不减。
            void forget(const Held& held) {
                eraseMatch(entries, held);
                eraseMatch(registry(), held);
            }

            std::vector<std::string> release() noexcept { return releaseEntries(entries); }

            // 移交给全局登记表，跨调用继续保持按下。
            void commit() {
                installExitGuardOnce();

                for (const auto& entry : entries) { // 重复按住同一个键不应在登记表里留下两条
                    eraseMatch(registry(), entry);
                }
                registry().insert(
                    registry().end(),
                    std::make_move_iterator(entries.begin()),
                    std::make_move_iterator(entries.end())
                );
                entries.clear();
            }
        };

        // --- 执行上下文与守卫 ---

        struct Runtime {
            HWND              hwnd = nullptr;
            RECT              client{};
            Options           options;
            Clock::time_point deadline;
            Ledger&           ledger;
        };

        Result<void> send(const Runtime& runtime, std::span<INPUT> events) {
            return runtime.options.dryRun || emit(events)
                     ? Result<void>{}
                     : Result<void>{fail(
                           ErrorCode::PartialFailure,
                           "SendInput did not deliver every event (last error " + std::to_string(GetLastError()) + ")."
                       )};
        }

        // 每一步之前复核：时间预算、窗口存活、前台归属。任一失效立即中止，
        // 避免后续事件打到用户真正在用的窗口上。
        Result<void> guard(const Runtime& runtime) {
            const bool expired = Clock::now() >= runtime.deadline;
            const bool alive   = IsWindow(runtime.hwnd) != FALSE;
            const bool focused = runtime.options.focus == FocusPolicy::Keep || GetForegroundWindow() == runtime.hwnd;

            return expired
                     ? Result<void>{fail(ErrorCode::DeadlineExceeded, "The input batch exceeded its time budget.")}
                 : !alive   ? Result<void>{fail(ErrorCode::GameExited, "The game window disappeared during the batch.")}
                 : !focused ? Result<void>{fail(
                                  ErrorCode::FocusLost,
                                  "The game window lost foreground during the batch: " + describeForeground() + "."
                              )}
                            : Result<void>{};
        }

        long long remainingMs(Clock::time_point until) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count();
        }

        // 分片休眠，长按期间守卫依然按时生效。dry_run 下不消耗真实时间。
        Result<void> settle(const Runtime& runtime, int milliseconds) {
            const auto until =
                Clock::now() + std::chrono::milliseconds(std::max(runtime.options.dryRun ? 0 : milliseconds, 0));
            Result<void> result{};
            for (long long left = remainingMs(until); left > 0 && result.has_value(); left = remainingMs(until)) {
                result = guard(runtime);
                Sleep(static_cast<DWORD>(std::min<long long>(left, SliceMs)));
            }
            return result;
        }

        // --- 按下 / 释放 ---

        Result<void> pressDown(Runtime& runtime, Held target) {
            INPUT event = heldEvent(target, false);
            return send(runtime, std::span{&event, 1}).transform([&] {
                if (!runtime.options.dryRun) runtime.ledger.add(std::move(target));
            });
        }

        Result<void> pressUp(Runtime& runtime, const Held& target) {
            INPUT event = heldEvent(target, true);
            return send(runtime, std::span{&event, 1}).transform([&] { runtime.ledger.forget(target); });
        }

        // 长按由 hold_ms 表达；action=down 则只按下不释放，交给后续步骤或 leave_held。
        Result<void> performOne(Runtime& runtime, const Held& target, PressAction action, int holdMs) {
            const auto cycle = [&] {
                return sequence(
                    [&] { return pressDown(runtime, target); },
                    [&] { return settle(runtime, holdMs); },
                    [&] { return pressUp(runtime, target); }
                );
            };

            switch (action) {
            case PressAction::Down:
                return pressDown(runtime, target);
            case PressAction::Up:
                return pressUp(runtime, target);
            case PressAction::Double:
                return sequence(cycle, [&] { return settle(runtime, DoubleGapMs); }, cycle);
            case PressAction::Press:
                break;
            }
            return cycle();
        }

        // 修饰键包住中间动作：按下顺序进入，释放逆序退出。
        template <typename Middle>
        Result<void> withHeld(Runtime& runtime, std::span<const Key> modifiers, Middle&& middle) {
            Result<void> result{};
            for (const auto& key : modifiers) {
                result = result.and_then([&] { return pressDown(runtime, asHeld(key)); });
            }
            result = result.and_then(std::forward<Middle>(middle));
            for (const auto& key : modifiers | std::views::reverse) {
                result = result.and_then([&] { return pressUp(runtime, asHeld(key)); });
            }
            return result;
        }

        Result<void> performKeys(Runtime& runtime, std::span<const Key> keys, PressAction action, int holdMs) {
            if (keys.empty()) return fail(ErrorCode::InvalidArgument, "A key step requires at least one key.");

            const auto holdAll = [&] {
                Result<void> result{};
                for (const auto& key : keys) result = result.and_then([&] { return pressDown(runtime, asHeld(key)); });
                return result;
            };
            const auto releaseAll = [&] {
                Result<void> result{};
                for (const auto& key : keys | std::views::reverse) {
                    result = result.and_then([&] { return pressUp(runtime, asHeld(key)); });
                }
                return result;
            };

            switch (action) {
            case PressAction::Down:
                return holdAll();
            case PressAction::Up:
                return releaseAll();
            default:
                break;
            }

            // 组合键：前置键作为修饰键按住，最后一个键承担实际动作。
            return withHeld(runtime, keys.first(keys.size() - 1), [&] {
                return performOne(runtime, asHeld(keys.back()), action, holdMs);
            });
        }

        // --- 坐标 ---

        // 百分比 -> 客户区物理像素。越界即刻拒绝，并把实际尺寸写进错误里。
        Result<Pixel> toClient(const Runtime& runtime, Point point) {
            const Pixel pixel{
                static_cast<int>(std::lround(point.x * runtime.client.right)),
                static_cast<int>(std::lround(point.y * runtime.client.bottom))
            };
            const bool inside =
                pixel.x >= 0 && pixel.y >= 0 && pixel.x < runtime.client.right && pixel.y < runtime.client.bottom;

            return inside ? Result<Pixel>{pixel}
                          : Result<Pixel>{fail(
                                ErrorCode::InvalidArgument,
                                "Coordinate (" + std::to_string(point.x) + ", " + std::to_string(point.y)
                                    + ") is outside the 0.0-1.0 range of the " + std::to_string(runtime.client.right)
                                    + "x" + std::to_string(runtime.client.bottom) + " client area."
                            )};
        }

        Result<void> moveTo(Runtime& runtime, Pixel client) {
            POINT screen{client.x, client.y};
            if (ClientToScreen(runtime.hwnd, &screen) == FALSE) {
                return fail(ErrorCode::GameExited, "The game window is no longer addressable.");
            }
            const auto event = absoluteMoveEvent(screen);
            if (!event.has_value()) {
                return fail(ErrorCode::Internal, "Virtual desktop metrics are unavailable.");
            }
            INPUT move = *event;
            return send(runtime, std::span{&move, 1});
        }

        Result<void> moveIfNeeded(Runtime& runtime, std::optional<Pixel> at) {
            return at.has_value() ? moveTo(runtime, *at) : Result<void>{};
        }

        Result<std::optional<Pixel>> resolve(const Runtime& runtime, const std::optional<Point>& at) {
            return at.has_value()
                     ? toClient(runtime, *at).transform([](Pixel pixel) { return std::optional<Pixel>{pixel}; })
                     : Result<std::optional<Pixel>>{std::optional<Pixel>{}};
        }

        // 拖拽必须产生中间移动事件：Minecraft 的分发式拖拽按经过的格子判定，
        // 只有起点和终点两个事件会被当成一次普通点击。
        Result<void> glide(Runtime& runtime, Pixel from, Pixel to, int segments) {
            const int    total = std::max(segments, 1);
            Result<void> result{};
            for (int step = 1; step <= total && result.has_value(); ++step) {
                const Pixel point{from.x + (to.x - from.x) * step / total, from.y + (to.y - from.y) * step / total};
                result =
                    sequence([&] { return moveTo(runtime, point); }, [&] { return settle(runtime, SegmentGapMs); });
            }
            return result;
        }

        // 视角转动走相对位移，累计取整避免分段丢失精度。
        Result<void> lookBy(Runtime& runtime, double dx, double dy, int segments) {
            const int    total = std::max(segments, 1);
            long long    sentX = 0;
            long long    sentY = 0;
            Result<void> result{};
            for (int step = 1; step <= total && result.has_value(); ++step) {
                const long long targetX = std::llround(dx * step / total);
                const long long targetY = std::llround(dy * step / total);
                INPUT event = relativeMoveEvent(static_cast<int>(targetX - sentX), static_cast<int>(targetY - sentY));
                sentX       = targetX;
                sentY       = targetY;
                result      = sequence(
                    [&] { return send(runtime, std::span{&event, 1}); },
                    [&] { return settle(runtime, SegmentGapMs); }
                );
            }
            return result;
        }

        Result<void> typeText(Runtime& runtime, const std::string& utf8) {
            if (utf8.empty()) return {};

            const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
            if (length <= 0) return fail(ErrorCode::InvalidArgument, "The text value is not valid UTF-8.");

            std::wstring wide(static_cast<std::size_t>(length), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);

            Result<void> result{};
            for (const wchar_t unit : wide) {
                INPUT events[]{unicodeEvent(unit, false), unicodeEvent(unit, true)};
                result = result.and_then([&] { return send(runtime, events); }).and_then([&] {
                    return settle(runtime, SegmentGapMs);
                });
            }
            return result;
        }

        // --- 步骤执行：每种步骤一个重载，由 std::visit 穷尽分发 ---

        struct Runner {
            Runtime& runtime;

            Result<std::optional<Pixel>> operator()(const MoveStep& step) const {
                return toClient(runtime, step.at).and_then([&](Pixel at) {
                    return moveTo(runtime, at).transform([at] { return std::optional<Pixel>{at}; });
                });
            }

            Result<std::optional<Pixel>> operator()(const ClickStep& step) const {
                return resolve(runtime, step.at).and_then([&](std::optional<Pixel> at) {
                    return sequence(
                               [&] { return moveIfNeeded(runtime, at); },
                               [&] {
                                   return withHeld(runtime, step.modifiers, [&] {
                                       return performOne(runtime, asHeld(step.button), step.action, step.holdMs);
                                   });
                               }
                    ).transform([at] { return at; });
                });
            }

            Result<std::optional<Pixel>> operator()(const DragStep& step) const {
                const auto from = toClient(runtime, step.from);
                const auto to   = toClient(runtime, step.to);
                return from.and_then([&](Pixel start) {
                    return to.and_then([&](Pixel end) -> Result<std::optional<Pixel>> {
                        return sequence(
                                   [&] { return moveTo(runtime, start); },
                                   [&] { return settle(runtime, SegmentGapMs); },
                                   [&] {
                                       return withHeld(runtime, step.modifiers, [&] {
                                           return sequence(
                                               [&] { return pressDown(runtime, asHeld(step.button)); },
                                               // 先停顿，游戏才会把按下判定落在起点上
                                               [&] { return settle(runtime, step.holdMs); },
                                               [&] { return glide(runtime, start, end, step.segments); },
                                               // 松开前再停顿，终点才会收到悬停
                                               [&] { return settle(runtime, step.holdMs); },
                                               [&] { return pressUp(runtime, asHeld(step.button)); }
                                           );
                                       });
                                   }
                        ).transform([end] { return std::optional<Pixel>{end}; });
                    });
                });
            }

            Result<std::optional<Pixel>> operator()(const ScrollStep& step) const {
                return resolve(runtime, step.at).and_then([&](std::optional<Pixel> at) {
                    return sequence(
                               [&] { return moveIfNeeded(runtime, at); },
                               [&] {
                                   return withHeld(runtime, step.modifiers, [&] {
                                       INPUT event = wheelEvent(step.amount, step.axis);
                                       return send(runtime, std::span{&event, 1});
                                   });
                               }
                    ).transform([at] { return at; });
                });
            }

            Result<std::optional<Pixel>> operator()(const LookStep& step) const {
                return lookBy(runtime, step.dx, step.dy, step.segments).transform([] {
                    return std::optional<Pixel>{};
                });
            }

            Result<std::optional<Pixel>> operator()(const KeyStep& step) const {
                return repeatTimes(
                           std::max(step.repeat, 1),
                           [&] {
                               return sequence(
                                   [&] { return performKeys(runtime, step.keys, step.action, step.holdMs); },
                                   [&] { return settle(runtime, SegmentGapMs); }
                               );
                           }
                ).transform([] { return std::optional<Pixel>{}; });
            }

            Result<std::optional<Pixel>> operator()(const TextStep& step) const {
                return typeText(runtime, step.value).transform([] { return std::optional<Pixel>{}; });
            }

            Result<std::optional<Pixel>> operator()(const WaitStep& step) const {
                return settle(runtime, step.ms).transform([] { return std::optional<Pixel>{}; });
            }
        };

        int elapsedMs(Clock::time_point since) {
            return static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count()
            );
        }

        Result<void> execute(Runtime& runtime, std::span<const Step> steps, Report& report) {
            Result<void> result{};
            for (std::size_t index = 0; index < steps.size() && result.has_value(); ++index) {
                const auto started = Clock::now();
                result             = guard(runtime)
                             .and_then([&] { return std::visit(Runner{runtime}, steps[index]); })
                             .transform([&](std::optional<Pixel> at) {
                                 report.steps.push_back({index, stepName(steps[index]), at, elapsedMs(started)});
                                 report.executed = index + 1;
                             })
                             .and_then([&] { return settle(runtime, runtime.options.stepDelayMs); });
            }
            return result;
        }

        // --- 前置检查：一张清单，逐项放行 ---

        struct Session {
            int        pid  = 0;
            HWND       hwnd = nullptr;
            RECT       client{};
            Options    options;
            WindowInfo info;
            bool       absoluteSteps = false;
        };

        // pid 为 0 时窗口查找会退化成“匹配任意 Minecraft 窗口”，必须先挡住。
        Result<void> checkProcess(Session& session) {
            return session.pid > 0 ? Result<void>{}
                                   : Result<void>{fail(
                                         ErrorCode::WindowNotFound,
                                         "The game process id is not set, so no game window exists yet."
                                     )};
        }

        Result<void> checkDesktop(Session&) {
            const HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
            if (desktop == nullptr) {
                return fail(
                    ErrorCode::DesktopUnavailable,
                    "No interactive desktop is available. The workstation may be locked or the RDP session detached."
                );
            }
            CloseDesktop(desktop);
            return {};
        }

        Result<void> checkWindow(Session& session) {
            session.hwnd = Detail::findMinecraftWindow(static_cast<DWORD>(session.pid));
            return session.hwnd != nullptr
                     ? Result<void>{}
                     : Result<void>{
                           fail(ErrorCode::WindowNotFound, "No Minecraft window was found for the game process.")
                       };
        }

        Result<void> checkMinimized(Session& session) {
            if (IsIconic(session.hwnd) == FALSE) return {};
            if (!session.options.restoreIfMinimized) {
                return fail(
                    ErrorCode::WindowMinimized,
                    "The game window is minimized and restore_if_minimized is off."
                );
            }
            ShowWindow(session.hwnd, SW_RESTORE);
            Sleep(RestoreWaitMs);
            return IsIconic(session.hwnd) == FALSE
                     ? Result<void>{}
                     : Result<void>{fail(ErrorCode::WindowMinimized, "The game window could not be restored.")};
        }

        std::optional<DWORD> integrityLevel(HANDLE process) {
            HANDLE rawToken = nullptr;
            if (OpenProcessToken(process, TOKEN_QUERY, &rawToken) == FALSE) return std::nullopt;

            ProcessHandle token{rawToken};
            DWORD         size = 0;
            GetTokenInformation(token.value, TokenIntegrityLevel, nullptr, 0, &size);
            if (size == 0) return std::nullopt;

            std::vector<std::byte> buffer(size);
            if (GetTokenInformation(token.value, TokenIntegrityLevel, buffer.data(), size, &size) == FALSE) {
                return std::nullopt;
            }

            auto*      label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer.data());
            const auto index = static_cast<DWORD>(*GetSidSubAuthorityCount(label->Label.Sid) - 1);
            return *GetSidSubAuthority(label->Label.Sid, index);
        }

        // UIPI 会静默丢弃投向更高完整性级别进程的输入，提前识别比事后猜测可靠。
        Result<void> checkPrivilege(Session& session) {
            ProcessHandle target{
                OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(session.pid))
            };
            const auto theirs  = target ? integrityLevel(target.value) : std::optional<DWORD>{};
            const auto ours    = integrityLevel(GetCurrentProcess());
            const bool blocked = !target || (theirs.has_value() && ours.has_value() && *theirs > *ours);

            return blocked ? Result<void>{fail(
                                 ErrorCode::PrivilegeBlocked,
                                 "The game process runs at a higher integrity level than MCDK; Windows blocks injected "
                                 "input. "
                                 "Restart MCDK with the same elevation as the game."
                             )}
                           : Result<void>{};
        }

        // 抢前台失败时，说清楚是谁占着，否则调用方只能猜。
        std::string describeForeground() {
            const HWND current = GetForegroundWindow();
            if (current == nullptr) return "no window currently holds the foreground";

            wchar_t   title[256]{};
            const int length = GetWindowTextW(current, title, static_cast<int>(std::size(title)));
            DWORD     pid    = 0;
            GetWindowThreadProcessId(current, &pid);

            std::string name(static_cast<std::size_t>(length) * 3 + 1, char{});
            const int   written = WideCharToMultiByte(
                CP_UTF8,
                0,
                title,
                length,
                name.data(),
                static_cast<int>(name.size()),
                nullptr,
                nullptr
            );
            name.resize(static_cast<std::size_t>(std::max(written, 0)));

            return "foreground is held by pid " + std::to_string(pid)
                 + (name.empty() ? std::string{} : " (\"" + name + "\")");
        }

        Result<void> acquireForeground(Session& session) {
            {
                // 绑定的对象是「当前占着前台的那个窗口的线程」，不是目标窗口。
                ForegroundAttach attach{GetForegroundWindow()};
                SetForegroundWindow(session.hwnd);
                BringWindowToTop(session.hwnd);
            }

            const auto until = Clock::now() + std::chrono::milliseconds(FocusWaitMs);
            for (; GetForegroundWindow() != session.hwnd && Clock::now() < until;) {
                Sleep(10);
            }

            session.info.foreground = GetForegroundWindow() == session.hwnd;
            if (!session.info.foreground) {
                return fail(
                    ErrorCode::FocusDenied,
                    "The game window could not be brought to the foreground: " + describeForeground()
                        + ". Windows blocks foreground changes while another process owns the last input event."
                );
            }

            Sleep(FocusSettleMs); // 游戏重新接管输入之前发出的事件会被吞掉
            return {};
        }

        Result<void> checkFocus(Session& session) {
            session.info.foreground = GetForegroundWindow() == session.hwnd;

            switch (session.options.focus) {
            case FocusPolicy::Keep:
                return {};
            case FocusPolicy::Require:
                return session.info.foreground
                         ? Result<void>{}
                         : Result<void>{fail(
                               ErrorCode::FocusDenied,
                               "The game window is not in the foreground and focus policy is require: "
                                   + describeForeground() + "."
                           )};
            case FocusPolicy::Auto:
                break;
            }
            return session.info.foreground ? Result<void>{} : acquireForeground(session);
        }

        Result<void> checkGeometry(Session& session) {
            if (GetClientRect(session.hwnd, &session.client) == FALSE) {
                return fail(ErrorCode::WindowNotFound, "The game window client area could not be measured.");
            }
            session.info.width  = session.client.right;
            session.info.height = session.client.bottom;

            return session.client.right > 0 && session.client.bottom > 0
                     ? Result<void>{}
                     : Result<void>{fail(ErrorCode::WindowMinimized, "The game window client area is empty.")};
        }

        // 游戏独占指针时会同时做两件事：隐藏光标，并把它限制在窗口内。
        // 只看「光标隐藏」会误判——Windows 默认开启的「打字时隐藏指针」也会隐藏光标，
        // 用户随便敲几下键盘就足以让后续所有带坐标的调用被拒绝。
        bool pointerHeldBy(HWND target) noexcept {
            CURSORINFO cursor{};
            cursor.cbSize = sizeof(cursor);
            if (GetCursorInfo(&cursor) == FALSE || (cursor.flags & CURSOR_SHOWING) != 0) return false;
            if (GetForegroundWindow() != target) return false; // 后台窗口不可能持有指针

            RECT clip{};
            if (GetClipCursor(&clip) == FALSE) return false;

            // 未被限制时，clip 覆盖整个虚拟桌面。
            const LONG left   = GetSystemMetrics(SM_XVIRTUALSCREEN);
            const LONG top    = GetSystemMetrics(SM_YVIRTUALSCREEN);
            const LONG right  = left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
            const LONG bottom = top + GetSystemMetrics(SM_CYVIRTUALSCREEN);

            return clip.left > left || clip.top > top || clip.right < right || clip.bottom < bottom;
        }

        Result<void> checkPointer(Session& session) {
            session.info.pointerLocked = pointerHeldBy(session.hwnd);
            session.info.imeOpen       = imeIsOpen(session.hwnd);

            return session.info.pointerLocked && session.absoluteSteps
                     ? Result<void>{fail(
                           ErrorCode::PointerModeMismatch,
                           "The game holds the pointer (in-world view), so absolute coordinates do not apply. "
                           "Use look for camera motion, click without at for in-place clicks, or press esc first."
                       )}
                     : Result<void>{};
        }

        using Check = Result<void> (*)(Session&);

        constexpr Check Preflight[]{
            &checkProcess,
            &checkDesktop,
            &checkWindow,
            &checkMinimized,
            &checkPrivilege,
            &checkFocus,
            &checkGeometry,
            &checkPointer,
        };

        Result<void> preflight(Session& session) {
            Result<void> result{};
            for (const auto check : Preflight) {
                result = result.and_then([&] { return check(session); });
            }
            return result;
        }

        // --- 批次串行化 ---

        using Lock = std::unique_lock<std::timed_mutex>;

        std::timed_mutex& inputMutex() {
            static std::timed_mutex mutex;
            return mutex;
        }

        Result<Lock> acquireLock() {
            Lock lock{inputMutex(), std::defer_lock};
            return lock.try_lock_for(std::chrono::milliseconds(LockWaitMs))
                     ? Result<Lock>{std::move(lock)}
                     : Result<Lock>{fail(ErrorCode::Busy, "Another input batch is still running.")};
        }
#endif
    } // namespace

#ifdef _WIN32
    Result<Report> run(int pid, std::span<const Step> steps, const Options& options) {
        return acquireLock().and_then([&]([[maybe_unused]] Lock lock) -> Result<Report> {
            DpiScope dpi;

            Options effective = options;
            effective.focus   = options.dryRun ? FocusPolicy::Keep : options.focus;

            Report report;
            report.total  = steps.size();
            report.dryRun = effective.dryRun;
            // 先回收超租的保留输入：即使这次调用随后失败，泄漏的按键也已经放开了。
            report.released = sweepExpired();

            Session session{pid, nullptr, {}, effective, {}, std::ranges::any_of(steps, &stepUsesAbsoluteCoords)};

            return preflight(session)
                .and_then([&]() -> Result<Report> {
                    report.window = session.info;

                    ImeScope      ime{session.hwnd, effective.ime == ImePolicy::Suppress && !effective.dryRun};
                    CursorRestore restore{effective.restoreCursor && !effective.dryRun};
                    Ledger        ledger;
                    Runtime       runtime{
                        session.hwnd,
                        session.client,
                        effective,
                        Clock::now() + std::chrono::milliseconds(effective.budgetMs),
                        ledger
                    };

                    // 副作用记账只有这一处：无论成功还是失败，报告都说明停在哪一步、
                    // 哪些输入已被释放、哪些仍然按着。
                    auto outcome = execute(runtime, steps, report);
                    if (effective.leaveHeld && outcome.has_value()) {
                        ledger.commit();
                    } else {
                        auto released = ledger.release();
                        report.released.insert(report.released.end(), released.begin(), released.end());
                    }
                    report.held = namesOf(registry());

                    return outcome.transform([&] { return std::move(report); });
                })
                .transform_error([&](Error error) {
                    error.progress = std::move(report);
                    return error;
                });
        });
    }

    Result<WindowInfo> inspect(int pid) {
        DpiScope dpi;

        Options options;
        options.focus              = FocusPolicy::Keep;
        options.restoreIfMinimized = false;

        Session session{pid, nullptr, {}, options, {}, false};
        return preflight(session).transform([&] { return session.info; });
    }

    // 释放绝不能有前置条件。键抬起本身是安全操作，拒绝执行永远比执行更糟：
    // 之前这里先跑 preflight，抢不到前台就直接返回，结果把按键永久留在了系统里。
    Result<Report> releaseHeld(int pid) {
        return acquireLock().and_then([&]([[maybe_unused]] Lock lock) -> Result<Report> {
            Report report;
            report.released = releaseEntries(registry());

            // 窗口状态只是附带的诊断信息，取不到也不影响释放本身。
            DpiScope dpi;
            Options  options;
            options.focus              = FocusPolicy::Keep;
            options.restoreIfMinimized = false;

            Session session{pid, nullptr, {}, options, {}, false};
            if (preflight(session).has_value()) {
                report.window = session.info;
            }
            return report;
        });
    }

    std::vector<std::string> heldKeys() { return namesOf(registry()); }
#else
    Result<Report> run(int, std::span<const Step>, const Options&) {
        return std::unexpected(Error{ErrorCode::Unsupported, "Input injection requires Windows.", {}});
    }

    Result<WindowInfo> inspect(int) {
        return std::unexpected(Error{ErrorCode::Unsupported, "Input injection requires Windows.", {}});
    }

    Result<Report> releaseHeld(int) {
        return std::unexpected(Error{ErrorCode::Unsupported, "Input injection requires Windows.", {}});
    }

    std::vector<std::string> heldKeys() { return {}; }
#endif
} // namespace MCDevTool::Input
