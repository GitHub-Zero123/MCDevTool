#pragma once

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace mcdk::jsonui_debugger {

    inline constexpr const char* ToolName = "jsonui_debugger";

    inline constexpr const char* ToolDescription =
        "Analyze native Minecraft JSON UI runtime state through a command string. "
        "Pass the command in the cmd argument; use cmd=\"/help\" to list available commands and usage. "
        "Supports screen listing, node lookup, shallow tree inspection, layout snapshots, and HTML-like pseudo output "
        "derived from Minecraft runtime layout/render data for layout reference only. "
        "Read-only by default with depth/node limits to avoid large UI dumps.";

    inline std::string trimCopy(std::string_view value) {
        std::size_t begin = 0;
        while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
            ++begin;
        }
        std::size_t end = value.size();
        while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
            --end;
        }
        return std::string(value.substr(begin, end - begin));
    }

    inline bool startsWith(std::string_view value, std::string_view prefix) {
        return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
    }

    inline std::string helpText(std::string_view command = {}) {
        if (command == "screens" || command == "/screens") {
            return R"(/screens
Return current native JSON UI screen state.

Output data:
- top_screen: current top screen short name.
- screens: full screen names such as hud.hud_screen.
- ui_size: JSON UI logical size.
- HUD overlays are usually exposed as hud.hud_screen and mounted under vanilla HUD parents, not under /.
- window_size: client window size.)";
        }
        if (command == "overview" || command == "/overview") {
            return R"(/overview [--screen=top|all|<screen>] [--child-limit=12] [--nud]
Return a compact starting point for AI UI inspection.

It lists current screens, probes common native JSON UI root candidates, returns direct child summaries, and suggests next commands. Use this before /tree or /html when you do not know the component path.

Options:
- HUD overlays are normally under hud.hud_screen with a long vanilla parent path; use /overview --screen=hud.hud_screen to get suggested_root.
- --nud: allow a short NetEase UI Debugger tree probe for the current top screen when normal native root candidates fail. This uses the official debugger event path, not Python ScreenNode state.)";
        }
        if (command == "mod-ui" || command == "/mod-ui") {
            return R"(/mod-ui [--include-registered] [--children-depth=1] [--limit=80]
List only loaded ModSDK custom UI ScreenNode entries from client.ui.uiManager.

This command is only an inventory of ModSDK-side custom UI loading state. It does not return native JSON UI controls, native layout data, runtime geometry, or the C++ UI tree.

Use /overview, /tree, /html, /render, /find, /node, or /children for native JSON UI analysis. Do not use /mod-ui as a fallback for native UI tree discovery.

Output data:
- native_screens: current native screen stack from gui.get_all_screen_fullnames().
- ui_screen_stack: ModSDK ScreenNode stack, including the hud.hud_screen root.
- ui_stack: loaded CreateUI custom UIs, commonly HUD overlays.
- ui_bind: entity/world-position bound UIs.
- pushed_screens: lazily wrapped PushScreen/native pushed screens.
- registered_ui: optional RegisterUI definitions when --include-registered is passed.
- native_component_path: runtime component_path for normal jsonui_debugger commands.
- netease_debugger_path: path for official NetEase UI Debugger gui.nud_* APIs only. Do not feed this into /tree, /html, /find, /node, or /children.

Useful next step:
- After a custom UI is identified here, use its screen_name and component_path with native commands such as /tree, /html, /render, or /find.)";
        }
        if (command == "reload-ui" || command == "/reload-ui") {
            return R"(/reload-ui [--preserve-mod-ui]
Trigger Minecraft's native Ctrl+R JSON UI definition reload from the host process.

This command belongs to jsonui_debugger because it is specifically for JSON UI hot-reload testing. It follows the same engine path as pressing Ctrl+R in game; it is not ui_editor.reload_ui_file and does not call ModSDK business-side UI APIs.

Options:
- --preserve-mod-ui: experimental ModSDK custom UI freeze/restore transaction. Before Ctrl+R, it freezes a temporary Python-side snapshot of user Addon UI registry entries and the currently loaded CreateUI/PushScreen stack, clears current ModSDK UI, then restores the saved registry and UI stack after UIDefReloadSceneStackAfter. It does not hook ModSDK source methods, does not broadcast UiInitFinished, and does not serialize complex UI params into C++.

Known behavior:
- The engine may reset pushed screens and mod HUD UI during reload.
- ModSDK Python ScreenNode state may become out of sync after the engine rebuilds JSON UI definitions.
- --preserve-mod-ui is intended to keep the visible Mod UI session alive across JSON UI reload by freezing and restoring a short-lived snapshot.
- UI-specific creation params are restored only when the UI explicitly stored a dict in _mcdk_reload_param. CreateUI uses a non-blocking HUD fallback param when missing; PushScreen uses None when missing.
- Use this only when intentionally testing JSON UI hot-reload behavior.)";
        }
        if (command == "probe" || command == "/probe") {
            return R"(/probe <screen> <path>
Check whether a node path can be read and return a compact summary.

Returns basic node data, computed geometry, visibility, type, and direct child count.)";
        }
        if (command == "children" || command == "/children") {
            return R"(/children <screen> <path> [--detail] [--limit=50]
List direct child nodes only.

Options:
- --detail: include basic data for each child.
- --limit=N: clamp returned children, default 50, max 200.)";
        }
        if (command == "node" || command == "/node") {
            return R"(/node <screen> <path> [--fields=basic,layout,text,container]
Return one node.

Options:
- --fields=basic,layout,text controls extra sections.
- basic is always returned.
- layout includes JSON UI layout expressions and anchors.
- text includes readable text-related properties when available.)";
        }
        if (command == "tree" || command == "/tree") {
            return R"(/tree <screen> <path> [--depth=2] [--max-nodes=80] [--visible-only]
Read a bounded native JSON UI tree.

Safety:
- Expands with get_children_name_from_parent level by level.
- Default depth is 2 and max nodes is 80.
- Complex mod screens can pass larger --depth and --max-nodes explicitly.
- For HUD overlays, start from the /overview suggested_root for hud.hud_screen; / is not an enumerable native HUD parent.
- --visible-only filters hidden nodes.)";
        }
        if (command == "html" || command == "/html") {
            return R"(/html <screen> <path> [--depth=2] [--max-nodes=80] [--visible-only] [--html-only]
Return the same bounded tree plus HTML-like pseudo output for AI layout reading.

The HTML-like output is derived from Minecraft runtime layout/render data. It is only a layout reference, not a source JSON UI reconstruction and not browser-accurate HTML.

Options:
- Complex mod screens can pass larger --depth and --max-nodes explicitly.
- For HUD overlays, start from the /overview suggested_root for hud.hud_screen; / is not an enumerable native HUD parent.
- --html-only omits the full tree and returns html plus a compact summary.)";
        }
        if (command == "render" || command == "/render") {
            return R"(/render <screen> <path> [--depth=2] [--max-nodes=80] [--visible-only] [--label=name|type|path-tail|none] [--legend=false] [--image] [--out=<absolute.svg>] [--unsafe-svg-image]
Return an SVG layout diagram derived from Minecraft runtime layout/render data.

This is mainly for users to visually inspect layout boxes. AI should prefer /overview, /html --html-only, and structured JSON unless it has strong image understanding.

Options:
- --label=name|type|path-tail|none controls rectangle labels, default name.
- --legend=false hides the color legend.
- --visible-only filters hidden nodes.
- Complex mod screens can pass larger --depth and --max-nodes explicitly.
- For HUD overlays, start from the /overview suggested_root for hud.hud_screen; / is not an enumerable native HUD parent.
- --image keeps a compact text fallback only. Direct SVG image content is disabled by default because some clients mix tool images into later model context and break subsequent chats.
- --out=<absolute.svg> writes the generated SVG to an absolute file path, creates parent directories, and returns only compact text plus svg_path.
- --unsafe-svg-image is accepted for backward compatibility but direct MCP image/svg+xml content is disabled by this server; use --out for user visual inspection.
- The SVG is not a screenshot and does not contain real textures.)";
        }
        if (command == "find" || command == "/find") {
            return R"(/find <screen> <path> <query> [--type=Button] [--match=name] [--depth=5] [--limit=30]
Search under a bounded root. Defaults to node-name matching to avoid parent path noise.

Options:
- --type=Button filters exact native control type name.
- --match=name|path|both controls where query is matched, default name.
- --depth=N bounds traversal depth.
- --limit=N bounds returned matches.
- --max-nodes=N bounds total visited nodes.)";
        }
        return R"(jsonui_debugger commands:
/help
/help <command>
/screens
/overview [--screen=top|all|<screen>] [--child-limit=12] [--nud]
/mod-ui [--include-registered] [--children-depth=1] [--limit=80]
/reload-ui [--preserve-mod-ui]
/probe <screen> <path>
/children <screen> <path> [--detail] [--limit=50]
/node <screen> <path> [--fields=basic,layout,text,container]
/tree <screen> <path> [--depth=2] [--max-nodes=80] [--visible-only]
/html <screen> <path> [--depth=2] [--max-nodes=80] [--visible-only] [--html-only]
/render <screen> <path> [--depth=2] [--max-nodes=80] [--visible-only] [--label=name|type|path-tail|none] [--image] [--out=<absolute.svg>]
/find <screen> <path> <query> [--type=Button] [--match=name] [--depth=5] [--limit=30]

Safety:
- Start with /overview when component paths are unknown.
- Use /mod-ui only to list ModSDK ScreenNode custom UI loading state, especially HUD overlays created by CreateUI. It must not be used as native JSON UI analysis or C++ UI tree fallback.
- /reload-ui intentionally triggers the native Ctrl+R UI definition reload and may reset pushed screens or mod HUD UI. Use /reload-ui --preserve-mod-ui to freeze a temporary ModSDK user UI snapshot and restore it after reload.
- For HUD overlays, use /overview --screen=hud.hud_screen; HUD controls are mounted under vanilla HUD parents and / is not enumerable.
- Tree commands expand level by level.
- Default /tree limits: depth=2, max-nodes=80.
- Complex mod screens can pass larger --depth and --max-nodes explicitly.
- /html is a runtime layout reference, not source JSON UI reconstruction.
- /render is mainly for human visual inspection. Use --out=<absolute.svg> to write a viewable SVG file. --image and --unsafe-svg-image both return compact text only.
- Avoid raw recursive JSON UI APIs on large roots.)";
    }

    inline bool commandHasFlag(std::string_view command, std::string_view flag) {
        std::size_t pos = 0;
        while ((pos = command.find(flag, pos)) != std::string_view::npos) {
            const bool leftOk  = pos == 0 || std::isspace(static_cast<unsigned char>(command[pos - 1]));
            const auto end     = pos + flag.size();
            const bool rightOk = end >= command.size() || std::isspace(static_cast<unsigned char>(command[end]));
            if (leftOk && rightOk) {
                return true;
            }
            pos = end;
        }
        return false;
    }

    inline std::optional<std::string> commandOptionValue(std::string_view command, std::string_view optionName) {
        const auto flag = std::string("--") + std::string(optionName) + "=";
        std::size_t pos = 0;
        while ((pos = command.find(flag, pos)) != std::string_view::npos) {
            const bool leftOk = pos == 0 || std::isspace(static_cast<unsigned char>(command[pos - 1]));
            if (!leftOk) {
                pos += flag.size();
                continue;
            }

            std::size_t valueStart = pos + flag.size();
            if (valueStart >= command.size()) {
                return std::string();
            }

            std::string out;
            const char first = command[valueStart];
            if (first == '"' || first == '\'') {
                const char quote = first;
                ++valueStart;
                bool escaped = false;
                for (std::size_t i = valueStart; i < command.size(); ++i) {
                    const char ch = command[i];
                    if (escaped) {
                        out.push_back(ch);
                        escaped = false;
                        continue;
                    }
                    if (ch == '\\') {
                        if (i + 1 < command.size() && (command[i + 1] == quote || command[i + 1] == '\\')) {
                            escaped = true;
                            continue;
                        }
                        out.push_back(ch);
                        continue;
                    }
                    if (ch == quote) {
                        return out;
                    }
                    out.push_back(ch);
                }
                return out;
            }

            std::size_t valueEnd = valueStart;
            while (valueEnd < command.size() && !std::isspace(static_cast<unsigned char>(command[valueEnd]))) {
                ++valueEnd;
            }
            return std::string(command.substr(valueStart, valueEnd - valueStart));
        }
        return std::nullopt;
    }

    inline std::string commandNameAfterHelp(std::string_view command) {
        std::string text = trimCopy(command);
        if (!startsWith(text, "/help")) {
            return {};
        }
        auto rest = trimCopy(std::string_view(text).substr(5));
        if (startsWith(rest, "/")) {
            rest.erase(rest.begin());
        }
        return rest;
    }

    inline nlohmann::json buildLocalHelpJson(std::string_view command) {
        const auto detail = commandNameAfterHelp(command);
        return nlohmann::json{{"ok", true}, {"cmd", "/help"}, {"data", {{"text", helpText(detail)}}}};
    }

    inline std::string jsonEscapeForPySingleQuotedString(std::string_view value) {
        std::string out;
        out.reserve(value.size() + 16);
        for (char ch : value) {
            switch (ch) {
            case '\\':
                out += "\\\\";
                break;
            case '\'':
                out += "\\'";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                out += ch;
                break;
            }
        }
        return out;
    }

    inline std::string buildPythonCode(std::string_view command) {
        // Keep command parsing in Python first: iteration is faster and avoids repeatedly changing MCP schema.
        // This code is intentionally self-contained so it can be sent through the existing execute_code IPC path.
        std::ostringstream py;
        py << "import json\n";
        py << "import gui\n";
        py << "cmd = '" << jsonEscapeForPySingleQuotedString(command) << "'\n";
        py << R"PY(

TYPE_NAMES = {
    -1: 'All', 0: 'Button', 1: 'Custom', 2: 'CollectionPanel', 3: 'Dropdown',
    4: 'EditBox', 5: 'Factory', 6: 'Grid', 7: 'Image', 8: 'InputPanel',
    9: 'Label', 10: 'Panel', 11: 'Screen', 12: 'ScrollbarBox', 13: 'ScrollTrack',
    14: 'ScrollView', 15: 'SelectionWheel', 16: 'Slider', 17: 'SliderBox',
    18: 'StackPanel', 19: 'Toggle', 20: 'ImageCycler', 21: 'LabelCycler',
    22: 'GridPageIndicator', 23: 'Combox', 24: 'Layout', 25: 'StackGrid',
    26: 'Joystick', 27: 'RichText', 28: 'SixteenNineLayout', 29: 'MulLinesEdit',
    30: 'AminProcessBar', 31: 'Unknown'
}

def _type_name(value):
    return TYPE_NAMES.get(value, 'EngineUnknown%s' % value)

def _as_list(value):
    if isinstance(value, tuple):
        return list(value)
    return value

def _ok(name, data):
    return {'ok': True, 'cmd': name, 'data': data}

def _err(name, message, code='ERROR'):
    return {'ok': False, 'cmd': name, 'error': {'code': code, 'message': message}}

def _split_args(s):
    # Small shell-like parser: supports spaces and quotes, enough for UI paths.
    args, cur, quote, esc = [], '', None, False
    for ch in s.strip().split('#', 1)[0]:
        if esc:
            cur += ch
            esc = False
            continue
        if ch == '\\':
            esc = True
            continue
        if quote:
            if ch == quote:
                quote = None
            else:
                cur += ch
            continue
        if ch in ('"', "'"):
            quote = ch
            continue
        if ch.isspace():
            if cur:
                args.append(cur)
                cur = ''
            continue
        cur += ch
    if cur:
        args.append(cur)
    return args

def _option(args, name, default=None):
    prefix = '--' + name + '='
    flag = '--' + name
    for item in args:
        if item.startswith(prefix):
            return item[len(prefix):]
        if item == flag:
            return True
    return default

def _int_option(args, name, default, min_value=None, max_value=None):
    try:
        value = int(_option(args, name, default))
    except Exception:
        value = default
    if min_value is not None:
        value = max(min_value, value)
    if max_value is not None:
        value = min(max_value, value)
    return value

def _string_option(args, name, default, allowed=None):
    value = _option(args, name, default)
    if value is True or value is None:
        value = default
    value = str(value)
    if allowed and value not in allowed:
        value = default
    return value

)PY";
        py << R"PY(

def _node_basic(screen, path):
    t = gui.get_control_def_type(screen, path)
    visible = False
    computed = {'size': None, 'position': None, 'global_position': None}
    try:
        visible = gui.get_visible(screen, path)
    except Exception:
        pass
    try:
        computed['size'] = _as_list(gui.get_size(screen, path))
    except Exception:
        pass
    try:
        computed['position'] = _as_list(gui.get_position(screen, path))
    except Exception:
        pass
    try:
        computed['global_position'] = _as_list(gui.get_global_position(screen, path))
    except Exception:
        pass
    data = {
        'screen': screen,
        'path': path,
        'name': path.rstrip('/').rsplit('/', 1)[-1] if path else '',
        'type_value': t,
        'type': _type_name(t),
        'visible': visible,
        'computed': computed
    }
    try:
        data['children_count'] = len(gui.get_children_name_from_parent(screen, path))
    except Exception:
        data['children_count'] = None
    return data

def _node_layout(screen, path):
    layout = {}
    for key, func in [
        ('size_x', gui.get_size_x), ('size_y', gui.get_size_y),
        ('position_x', gui.get_position_x), ('position_y', gui.get_position_y),
        ('anchor_from', gui.get_anchor_from), ('anchor_to', gui.get_anchor_to),
        ('clips_children', gui.get_clips_children),
    ]:
        try:
            layout[key] = func(screen, path)
        except Exception as exc:
            layout[key + '_error'] = repr(exc)
    return layout

def _node_text(screen, path):
    text = {}
    for key, func in [
        ('text', gui.get_text), ('text_color', gui.get_text_color),
        ('text_alignment', gui.get_text_alignment), ('text_shadow', gui.get_text_shadow),
        ('text_line_padding', gui.get_text_line_padding),
    ]:
        try:
            text[key] = _as_list(func(screen, path))
        except Exception:
            pass
    return text

def _children(screen, path, limit, detail):
    names = gui.get_children_name_from_parent(screen, path)
    if names is None:
        return {
            'screen': screen,
            'path': path,
            'children_count': None,
            'children': [],
            'truncated': False,
            'note': 'get_children_name_from_parent returned None; this path is not an enumerable native JSON UI parent.'
        }
    out = []
    for name in names[:limit]:
        child_path = path.rstrip('/') + '/' + name
        item = {'name': name, 'path': child_path}
        if detail:
            try:
                item.update(_node_basic(screen, child_path))
            except Exception as exc:
                item['error'] = repr(exc)
        out.append(item)
    return {
        'screen': screen,
        'path': path,
        'children_count': len(names),
        'children': out,
        'truncated': len(names) > limit
    }

ROOT_CANDIDATES = [
    '/variables_button_mappings_and_controls/safezone_screen_matrix/inner_matrix/safezone_screen_panel/root_screen_panel',
    '/variables_button_mappings_and_controls/safezone_screen_matrix/inner_matrix/safezone_screen_panel/root_screen_panel/root_panel',
    '/variables_button_mappings_and_controls/safezone_screen_matrix/inner_matrix/safezone_screen_panel/root_screen_panel/stack_panel',
    '/variables_button_mappings_and_controls/safezone_screen_matrix/inner_matrix/safezone_screen_panel/root_screen_panel/pause_screen_main_panels',
    '/variables_button_mappings_and_controls/safezone_screen_matrix/inner_matrix/safezone_screen_panel/root_screen_panel/base_panel',
    '/variables_button_mappings_and_controls/safezone_screen_matrix/inner_matrix/safezone_screen_panel/root_screen_panel/content_stack_panel',
    '/variables_button_mappings_and_controls/safezone_screen_matrix/inner_matrix/safezone_screen_panel/root_screen_panel/bg',
]

def _child_summary(screen, path, limit):
    try:
        names = gui.get_children_name_from_parent(screen, path)
    except Exception as exc:
        return {'ok': False, 'error': repr(exc), 'children': [], 'children_count': None, 'truncated': False}
    if names is None:
        return {
            'ok': False,
            'error': 'get_children_name_from_parent returned None',
            'children': [],
            'children_count': None,
            'truncated': False
        }
    children = []
    visible_count = 0
    hidden_count = 0
    for name in names[:limit]:
        child_path = path.rstrip('/') + '/' + name
        item = {'name': name, 'path': child_path}
        try:
            item['type'] = _type_name(gui.get_control_def_type(screen, child_path))
        except Exception as exc:
            item['type_error'] = repr(exc)
        try:
            item['visible'] = gui.get_visible(screen, child_path)
            if item['visible']:
                visible_count += 1
            else:
                hidden_count += 1
        except Exception as exc:
            item['visible_error'] = repr(exc)
        try:
            item['size'] = _as_list(gui.get_size(screen, child_path))
        except Exception as exc:
            item['size_error'] = repr(exc)
        children.append(item)
    if len(names) > limit:
        hidden_count = None
    return {
        'ok': True,
        'children_count': len(names),
        'children': children,
        'visible_in_sample': visible_count,
        'hidden_in_sample': hidden_count,
        'truncated': len(names) > limit
    }

def _probe_root_candidate(screen, path, child_limit):
    try:
        node = _node_basic(screen, path)
    except Exception as exc:
        return {'path': path, 'ok': False, 'error': repr(exc)}
    try:
        summary = _child_summary(screen, path, child_limit)
        ok = summary.get('ok') and summary.get('children_count') is not None
        if not ok:
            return {'path': path, 'ok': False, 'node': node, 'error': summary.get('error')}
        result = {
            'path': path,
            'ok': True,
            'type': node.get('type'),
            'visible': node.get('visible'),
            'computed': node.get('computed'),
            'children_count': summary.get('children_count'),
            'children': summary.get('children'),
            'visible_in_sample': summary.get('visible_in_sample'),
            'hidden_in_sample': summary.get('hidden_in_sample'),
            'truncated': summary.get('truncated')
        }
        return result
    except Exception as exc:
        return {'path': path, 'ok': False, 'node': node, 'error': repr(exc)}

def _screen_short_to_full(short_name, screens):
    for item in screens:
        if item == short_name or item.endswith('.' + short_name):
            return item
    return short_name

def _nud_extract_tree_from_event(args):
    try:
        raw = args.get('data') if isinstance(args, dict) else None
        obj = json.loads(raw) if isinstance(raw, str) or raw.__class__.__name__ in ('unicode', 'str') else raw
        if not isinstance(obj, dict) or not obj.get('success'):
            return None
        data = obj.get('data')
        if isinstance(data, dict):
            data = data.get('data')
        if isinstance(data, dict) and data.get('type') == 'screen':
            return data
    except Exception:
        return None
    return None

def _nud_root_candidates_from_tree(tree):
    out = []
    seen = set()
    for child in tree.get('controls', []) if isinstance(tree, dict) else []:
        if not isinstance(child, dict):
            continue
        name = child.get('name')
        if not name or '/' in name:
            continue
        path = '/' + name
        if path in seen:
            continue
        seen.add(path)
        out.append({
            'path': path,
            'name': name,
            'type': child.get('type'),
            'visible': child.get('visible'),
            'children_count_hint': len(child.get('controls', [])) if isinstance(child.get('controls'), list) else None
        })
    return out

def _discover_screen_roots_with_nud(screen, child_limit):
    # The official NetEase UI debugger can see the screen-level control tree. Its API returns data through
    # UIDebuggerNotifyEvent, so keep this as a short fallback transaction and validate candidates with normal APIs.
    try:
        from common import eventUtil
    except Exception as exc:
        return {'ok': False, 'error': 'eventUtil import failed: ' + repr(exc), 'candidate_roots': [], 'validated_roots': []}

    class _NudProbe(object):
        def __init__(self):
            self.events = []
        def on_event(self, args):
            self.events.append(args)

    probe = _NudProbe()
    old_enabled = False
    old_known = False
    out = {'ok': False, 'candidate_roots': [], 'validated_roots': [], 'used_root_path': '/'}
    try:
        try:
            old_enabled = gui.get_netease_ui_debugger_enable()
            old_known = True
        except Exception:
            pass
        eventUtil.instance.ListenForEngineClient('UIDebuggerNotifyEvent', probe, probe.on_event)
        gui.set_netease_ui_debugger_enable(True)
        gui.nud_get_control_tree('/')
    except Exception as exc:
        out['error'] = repr(exc)
    finally:
        try:
            eventUtil.instance.UnListenForEngineClient('UIDebuggerNotifyEvent', probe, probe.on_event)
        except Exception:
            pass
        try:
            gui.nud_set_selected_controls(json.dumps([]))
        except Exception:
            pass
        try:
            gui.nud_set_bounds_visible(False)
        except Exception:
            pass
        try:
            gui.set_netease_ui_debugger_enable(old_enabled if old_known and old_enabled else False)
        except Exception:
            pass

    tree = None
    for event_args in probe.events:
        tree = _nud_extract_tree_from_event(event_args)
        if tree:
            break
    if not tree:
        out['event_count'] = len(probe.events)
        out.setdefault('error', 'No control tree was returned through UIDebuggerNotifyEvent.')
        return out

    out['ok'] = True
    out['screen_node'] = {'name': tree.get('name'), 'type': tree.get('type'), 'visible': tree.get('visible')}
    out['candidate_roots'] = _nud_root_candidates_from_tree(tree)
    for candidate in out['candidate_roots']:
        try:
            root = _probe_root_candidate(screen, candidate.get('path'), child_limit)
            if root.get('ok'):
                root['discovered_by'] = 'netease_ui_debugger_tree'
                root['debugger_hint'] = candidate
                out['validated_roots'].append(root)
        except Exception as exc:
            candidate['validation_error'] = repr(exc)
    return out

def _overview(screen_arg, child_limit, allow_nud=False):
    screens = gui.get_all_screen_fullnames()
    top_short = gui.get_top_screen()
    top_full = _screen_short_to_full(top_short, screens)
    if screen_arg == 'all':
        target_screens = screens
    elif screen_arg == 'top':
        target_screens = [top_full] if top_full else []
    else:
        target_screens = [_screen_short_to_full(screen_arg, screens)]
    out_screens = []
    for screen in target_screens:
        roots = []
        root_discovery = None
        for path in ROOT_CANDIDATES:
            try:
                root = _probe_root_candidate(screen, path, child_limit)
                if root.get('ok'):
                    roots.append(root)
            except Exception:
                pass
        if allow_nud and not roots and screen == top_full:
            root_discovery = _discover_screen_roots_with_nud(screen, child_limit)
            roots.extend(root_discovery.get('validated_roots', []))
        suggested_root = None
        if roots:
            visible_roots = [r for r in roots if r.get('visible')]
            suggested_root = (visible_roots or roots)[0].get('path')
        screen_overview = {
            'screen': screen,
            'is_top': screen == top_full,
            'roots': roots,
            'suggested_root': suggested_root,
            'suggested_next': [
                '/html %s %s --depth=2 --max-nodes=80 --visible-only --html-only' % (screen, suggested_root),
                '/find %s %s <query> --depth=5 --limit=30' % (screen, suggested_root),
                '/children %s %s --detail --limit=30' % (screen, suggested_root)
            ] if suggested_root else []
        }
        if root_discovery is not None:
            screen_overview['root_discovery'] = root_discovery
        out_screens.append(screen_overview)
    return {
        'top_screen': top_short,
        'top_screen_fullname': top_full,
        'screens': screens,
        'ui_size': _as_list(gui.get_client_ui_screen_size()),
        'window_size': _as_list(gui.get_client_screen_size()),
        'child_limit': child_limit,
        'screen_overviews': out_screens,
        'nud_enabled': allow_nud,
        'note': 'Use suggested_root as the starting component_path. Do not use / as a native JSON UI root. Use --nud only when you need the official NetEase UI Debugger tree fallback; never infer roots from Python ScreenNode state.'
    }

)PY";
        py << R"PY(

def _tree(screen, root, max_depth, max_nodes, visible_only, include_text=False):
    scan_limit = max(max_nodes * 5, max_nodes + 50)
    counters = {'scanned': 0, 'returned': 0, 'truncated': False, 'reason': None}

    def walk_py2(path, depth):
        if counters['returned'] >= max_nodes:
            counters['truncated'] = True
            counters['reason'] = 'max_nodes'
            return None
        if counters['scanned'] >= scan_limit:
            counters['truncated'] = True
            counters['reason'] = 'scan_limit'
            return None
        counters['scanned'] += 1
        try:
            node = _node_basic(screen, path)
            node['layout'] = _node_layout(screen, path)
            if include_text and node.get('type') in ('Button', 'EditBox', 'Label', 'RichText', 'Toggle'):
                text_data = _node_text(screen, path)
                if text_data:
                    node['text'] = text_data
        except Exception as exc:
            return {'path': path, 'error': repr(exc), 'children': []}
        if visible_only and not node.get('visible', False):
            if depth >= max_depth:
                return None
            try:
                names = gui.get_children_name_from_parent(screen, path) or []
            except Exception:
                names = []
            visible_children = []
            for name in names:
                child = walk_py2(path.rstrip('/') + '/' + name, depth + 1)
                if child is not None:
                    visible_children.append(child)
                if counters['returned'] >= max_nodes or counters['scanned'] >= scan_limit:
                    counters['truncated'] = True
                    counters['reason'] = 'max_nodes' if counters['returned'] >= max_nodes else 'scan_limit'
                    break
            if visible_children:
                return {'path': path, 'filtered_hidden': True, 'children': visible_children}
            return None
        counters['returned'] += 1
        node['depth'] = depth
        node['children'] = []
        if depth >= max_depth:
            try:
                if gui.get_children_name_from_parent(screen, path):
                    counters['truncated'] = True
                    counters['reason'] = counters['reason'] or 'max_depth'
            except Exception:
                pass
            return node
        try:
            names = gui.get_children_name_from_parent(screen, path) or []
        except Exception:
            names = []
        for name in names:
            child_path = path.rstrip('/') + '/' + name
            child = walk_py2(child_path, depth + 1)
            if child is not None:
                if child.get('filtered_hidden'):
                    node['children'].extend(child.get('children', []))
                else:
                    node['children'].append(child)
            if counters['returned'] >= max_nodes or counters['scanned'] >= scan_limit:
                counters['truncated'] = True
                counters['reason'] = 'max_nodes' if counters['returned'] >= max_nodes else 'scan_limit'
                break
        return node

    tree = walk_py2(root, 0)
    return {
        'screen': screen,
        'root': root,
        'tree': tree,
        'truncated': counters['truncated'],
        'truncated_reason': counters['reason'],
        'visited_nodes': counters['scanned'],
        'scanned_nodes': counters['scanned'],
        'returned_nodes': counters['returned'],
        'max_depth': max_depth,
        'max_nodes': max_nodes,
        'scan_limit': scan_limit
    }

def _match_node(node, query, type_filter, match_mode):
    q = query.lower()
    if type_filter and node.get('type') != type_filter:
        return False
    in_name = q in node.get('name', '').lower()
    in_path = q in node.get('path', '').lower()
    if match_mode == 'path':
        return in_path
    if match_mode == 'both':
        return in_name or in_path
    return in_name

def _find(screen, root, query, type_filter, match_mode, max_depth, max_nodes, limit, visible_only):
    counters = {'visited': 0, 'truncated': False, 'reason': None}
    matches = []

    def walk(path, depth):
        if counters['visited'] >= max_nodes or len(matches) >= limit:
            counters['truncated'] = True
            counters['reason'] = 'max_nodes' if counters['visited'] >= max_nodes else 'limit'
            return
        counters['visited'] += 1
        try:
            node = _node_basic(screen, path)
        except Exception:
            return
        if (not visible_only or node.get('visible', False)) and _match_node(node, query, type_filter, match_mode):
            matches.append(node)
            if len(matches) >= limit:
                counters['truncated'] = True
                counters['reason'] = 'limit'
                return
        if depth >= max_depth:
            return
        try:
            names = gui.get_children_name_from_parent(screen, path) or []
        except Exception:
            names = []
        for name in names:
            walk(path.rstrip('/') + '/' + name, depth + 1)
            if counters['truncated']:
                break

    walk(root, 0)
    return {
        'screen': screen,
        'root': root,
        'query': query,
        'type_filter': type_filter,
        'match': match_mode,
        'matches': matches,
        'visited_nodes': counters['visited'],
        'returned_nodes': len(matches),
        'truncated': counters['truncated'],
        'truncated_reason': counters['reason'],
        'max_depth': max_depth,
        'max_nodes': max_nodes,
        'limit': limit
    }

)PY";
        py << R"PY(

def _screens():
    screens = gui.get_all_screen_fullnames()
    top_short = gui.get_top_screen()
    return {
        'top_screen': top_short,
        'top_screen_fullname': _screen_short_to_full(top_short, screens),
        'screens': screens,
        'ui_size': _as_list(gui.get_client_ui_screen_size()),
        'window_size': _as_list(gui.get_client_screen_size())
    }

def _safe_attr(obj, name, default=None):
    try:
        return getattr(obj, name, default)
    except Exception as exc:
        return {'error': repr(exc)}

def _safe_call(obj, name, default=None):
    try:
        func = getattr(obj, name)
        return func()
    except Exception as exc:
        return default

def _netease_debugger_root_name(screen_name):
    try:
        if not screen_name:
            return ''
        if '.' in screen_name:
            return screen_name.split('.')[-1]
        return screen_name
    except Exception:
        return ''

def _netease_debugger_path(screen_name, native_path):
    # NetEase UI Debugger paths include the debugger screen root:
    # hud.hud_screen + /variables... => /hud_screen/variables...
    root = _netease_debugger_root_name(screen_name)
    if not root or not native_path:
        return None
    if not native_path.startswith('/'):
        native_path = '/' + native_path
    return '/' + root + native_path

def _modsdk_node_info(node, children_depth=0, limit=80):
    cls = node.__class__
    data = {
        'class': getattr(cls, '__name__', ''),
        'module': getattr(cls, '__module__', _safe_attr(node, '__module__', '')),
        'namespace': _safe_attr(node, 'namespace', None),
        'name': _safe_attr(node, 'name', None),
        'full_name': _safe_attr(node, 'full_name', None),
        'screen_name': _safe_attr(node, 'screen_name', None),
        'component_path': _safe_attr(node, 'component_path', None),
        'def_key': _safe_attr(node, 'def_key', None),
        'org_key': _safe_attr(node, 'org_key', None),
        'input_mode': _safe_attr(node, 'input_mode', None),
        'is_push_screen': _safe_attr(node, 'is_push_screen', None),
        'visible': _safe_attr(node, 'visible', None),
        'enable': _safe_attr(node, 'enable', None),
        'removed': _safe_attr(node, 'removed', None),
        'ui_id': _safe_call(node, 'GetUiId', None),
        'bind_entity': _safe_call(node, 'GetBindEntityId', None),
        'bind_position': _safe_call(node, 'GetBindWorldPosition', None),
    }
    children = _safe_attr(node, 'children', []) or []
    try:
        data['children_count'] = len(children)
    except Exception:
        data['children_count'] = None
    if data.get('screen_name') and data.get('component_path'):
        native_path = data['component_path']
        if native_path and not native_path.startswith('/'):
            native_path = '/' + native_path
        data['native_component_path'] = native_path
        data['netease_debugger_path'] = _netease_debugger_path(data['screen_name'], native_path)
        data['path_semantics'] = {
            'native_component_path': 'Use with jsonui_debugger runtime commands: /tree, /html, /find, /node, /children, /probe.',
            'netease_debugger_path': 'Use only with official NetEase UI Debugger gui.nud_* APIs. It includes the debugger screen root.',
            'do_not_mix_paths': True
        }
        data['next_commands'] = [
            '/tree %s %s --depth=4 --max-nodes=300 --visible-only' % (data['screen_name'], native_path),
            '/html %s %s --depth=4 --max-nodes=300 --visible-only --html-only' % (data['screen_name'], native_path),
            '/find %s %s <query> --depth=6 --limit=30' % (data['screen_name'], native_path),
        ]
    if children_depth > 0:
        child_items = []
        for child in children[:limit]:
            child_items.append(_modsdk_node_info(child, children_depth - 1, limit))
        data['children'] = child_items
        data['children_truncated'] = len(children) > limit
    return data

def _modsdk_ui_inventory(include_registered=False, children_depth=1, limit=80):
    try:
        import client.ui.uiManager as uiManager
    except Exception as exc:
        return {
            'available': False,
            'error': repr(exc),
            'note': 'client.ui.uiManager is unavailable in this runtime.'
        }
    mgr = uiManager.instance()
    native_screens = []
    top_screen = None
    try:
        native_screens = gui.get_all_screen_fullnames()
        top_screen = gui.get_top_screen()
    except Exception:
        pass
    screen_def = _safe_attr(mgr, 'screen_def', {}) or {}
    ui_screen_stack = _safe_attr(mgr, '_ui_screen_stack', []) or []
    ui_stack = _safe_attr(mgr, '_ui_stack', []) or []
    ui_bind = _safe_attr(mgr, '_ui_bind', {}) or {}
    pushed_screens = _safe_attr(mgr, '_pushed_screens', {}) or {}
    data = {
        'available': True,
        'source': 'client.ui.uiManager.instance()',
        'semantic': 'Only ModSDK Python ScreenNode custom UI loading inventory. Not native JSON UI controls, not runtime layout data, and not authoritative C++ UI tree semantics.',
        'native_analysis_note': 'For native UI analysis use /overview, /tree, /html, /render, /find, /node, or /children with screen_name and component_path.',
        'top_screen': top_screen,
        'native_screens': native_screens,
        'manager': {
            'class': mgr.__class__.__name__,
            'module': getattr(mgr.__class__, '__module__', ''),
            'mIsInit': _safe_attr(mgr, 'mIsInit', None),
            'mInputMode': _safe_attr(mgr, 'mInputMode', None),
            'screen_def_count': len(screen_def),
        },
        'ui_screen_stack': [_modsdk_node_info(n, children_depth, limit) for n in ui_screen_stack[:limit]],
        'ui_screen_stack_truncated': len(ui_screen_stack) > limit,
        'ui_stack': [_modsdk_node_info(n, children_depth, limit) for n in ui_stack[:limit]],
        'ui_stack_truncated': len(ui_stack) > limit,
        'ui_bind': [{'key': repr(k), 'node': _modsdk_node_info(v, children_depth, limit)} for k, v in list(ui_bind.items())[:limit]],
        'ui_bind_truncated': len(ui_bind) > limit,
        'pushed_screens': [{'key': repr(k), 'node': _modsdk_node_info(v, children_depth, limit)} for k, v in list(pushed_screens.items())[:limit]],
        'pushed_screens_truncated': len(pushed_screens) > limit,
    }
    if include_registered:
        registered = []
        for key in sorted(screen_def.keys())[:limit]:
            value = screen_def.get(key)
            item = {'key': key}
            try:
                item.update({
                    'class_path': value[0] if len(value) > 0 else None,
                    'screen_full_name': value[1] if len(value) > 1 else None,
                    'def_key': value[2] if len(value) > 2 else None,
                    'org_key': value[3] if len(value) > 3 else None,
                })
            except Exception:
                item['value_repr'] = repr(value)
            registered.append(item)
        data['registered_ui'] = registered
        data['registered_ui_truncated'] = len(screen_def) > limit
    return data

)PY";
        py << R"PY(

MAX_TRAVERSAL_DEPTH = 64
MAX_TREE_NODES = 5000
MAX_FIND_NODES = 5000

def _run(cmd):
    args = _split_args(cmd)
    if not args or args[0] == '/help':
        return _ok('/help', {'text': 'Commands: /help, /screens, /overview, /mod-ui, /reload-ui, /probe, /children, /node, /tree, /html, /find'})
    name = args[0]
    if name == '/screens':
        return _ok(name, _screens())
    if name == '/overview':
        screen_arg = args[1] if len(args) >= 2 and not args[1].startswith('--') else _string_option(args[1:], 'screen', 'top')
        child_limit = _int_option(args[1:], 'child-limit', 12, 1, 50)
        allow_nud = bool(_option(args[1:], 'nud', False))
        return _ok(name, _overview(screen_arg, child_limit, allow_nud))
    if name == '/mod-ui':
        include_registered = bool(_option(args[1:], 'include-registered', False))
        children_depth = _int_option(args[1:], 'children-depth', 1, 0, 4)
        limit = _int_option(args[1:], 'limit', 80, 1, 500)
        return _ok(name, _modsdk_ui_inventory(include_registered, children_depth, limit))
    if name == '/probe':
        if len(args) < 3:
            return _err(name, 'usage: /probe <screen> <path>', 'USAGE')
        screen, path = args[1], args[2]
        data = _node_basic(screen, path)
        return _ok(name, data)
    if name == '/children':
        if len(args) < 3:
            return _err(name, 'usage: /children <screen> <path> [--detail] [--limit=50]', 'USAGE')
        limit = _int_option(args[3:], 'limit', 50, 1, 200)
        detail = bool(_option(args[3:], 'detail', False))
        return _ok(name, _children(args[1], args[2], limit, detail))
    if name == '/node':
        if len(args) < 3:
            return _err(name, 'usage: /node <screen> <path> [--fields=basic,layout,text]', 'USAGE')
        screen, path = args[1], args[2]
        fields = str(_option(args[3:], 'fields', 'basic,layout')).split(',')
        data = _node_basic(screen, path)
        if 'layout' in fields:
            data['layout'] = _node_layout(screen, path)
        if 'text' in fields:
            data['text'] = _node_text(screen, path)
        return _ok(name, data)
    if name == '/tree':
        if len(args) < 3:
            return _err(name, 'usage: /tree <screen> <path> [--depth=2] [--max-nodes=80]', 'USAGE')
        depth = _int_option(args[3:], 'depth', 2, 0, MAX_TRAVERSAL_DEPTH)
        max_nodes = _int_option(args[3:], 'max-nodes', 80, 1, MAX_TREE_NODES)
        visible_only = bool(_option(args[3:], 'visible-only', False))
        return _ok(name, _tree(args[1], args[2], depth, max_nodes, visible_only))
    if name == '/html':
        if len(args) < 3:
            return _err(name, 'usage: /html <screen> <path> [--depth=2] [--max-nodes=80]', 'USAGE')
        depth = _int_option(args[3:], 'depth', 2, 0, MAX_TRAVERSAL_DEPTH)
        max_nodes = _int_option(args[3:], 'max-nodes', 80, 1, MAX_TREE_NODES)
        visible_only = bool(_option(args[3:], 'visible-only', False))
        data = _tree(args[1], args[2], depth, max_nodes, visible_only, True)
        return _ok(name, data)
    if name == '/render':
        if len(args) < 3:
            return _err(name, 'usage: /render <screen> <path> [--depth=2] [--max-nodes=80]', 'USAGE')
        depth = _int_option(args[3:], 'depth', 2, 0, MAX_TRAVERSAL_DEPTH)
        max_nodes = _int_option(args[3:], 'max-nodes', 80, 1, MAX_TREE_NODES)
        visible_only = bool(_option(args[3:], 'visible-only', False))
        data = _tree(args[1], args[2], depth, max_nodes, visible_only, True)
        data['render_label'] = _string_option(args[3:], 'label', 'name', ('name', 'type', 'path-tail', 'none'))
        data['render_legend'] = _string_option(args[3:], 'legend', 'true', ('true', 'false')) != 'false'
        return _ok(name, data)
    if name == '/find':
        if len(args) < 4:
            return _err(name, 'usage: /find <screen> <path> <query> [--type=Button] [--match=name] [--depth=5] [--limit=30]', 'USAGE')
        depth = _int_option(args[4:], 'depth', 5, 0, MAX_TRAVERSAL_DEPTH)
        max_nodes = _int_option(args[4:], 'max-nodes', 300, 1, MAX_FIND_NODES)
        limit = _int_option(args[4:], 'limit', 30, 1, 100)
        type_filter = _option(args[4:], 'type', None)
        match_mode = _string_option(args[4:], 'match', 'name', ('name', 'path', 'both'))
        visible_only = bool(_option(args[4:], 'visible-only', False))
        return _ok(name, _find(args[1], args[2], args[3], type_filter, match_mode, depth, max_nodes, limit, visible_only))
    return _err(name, 'unknown command: ' + name, 'UNKNOWN_COMMAND')

try:
    _result = json.dumps(_run(cmd), ensure_ascii=False)
except Exception as exc:
    _result = json.dumps({'ok': False, 'cmd': cmd, 'error': {'code': 'EXCEPTION', 'message': repr(exc)}}, ensure_ascii=False)
)PY";
        return py.str();
    }

    inline std::optional<std::string> extractFirstJsonText(std::string_view text) {
        const auto start = text.find_first_of("{[");
        if (start == std::string_view::npos) {
            return std::nullopt;
        }

        std::vector<char> stack;
        bool              inStr = false;
        bool              esc   = false;

        for (std::size_t i = start; i < text.size(); ++i) {
            const char ch = text[i];
            if (inStr) {
                if (esc) {
                    esc = false;
                } else if (ch == '\\') {
                    esc = true;
                } else if (ch == '"') {
                    inStr = false;
                }
                continue;
            }
            if (ch == '"') {
                inStr = true;
                continue;
            }
            if (ch == '{') {
                stack.push_back('}');
            } else if (ch == '[') {
                stack.push_back(']');
            } else if (ch == '}' || ch == ']') {
                if (stack.empty() || stack.back() != ch) {
                    return std::nullopt;
                }
                stack.pop_back();
                if (stack.empty()) {
                    return std::string(text.substr(start, i - start + 1));
                }
            }
        }

        return std::nullopt;
    }

    inline nlohmann::json parseJsonMaybeNestedString(std::string_view text) {
        auto parsed = nlohmann::json::parse(text, nullptr, false);
        if (parsed.is_discarded()) {
            return parsed;
        }
        if (parsed.is_string()) {
            auto nested = nlohmann::json::parse(parsed.get<std::string>(), nullptr, false);
            if (!nested.is_discarded()) {
                return nested;
            }
        }
        return parsed;
    }

    inline std::optional<std::string> extractJsonStringLiteralAtStart(std::string_view text) {
        std::size_t start = 0;
        while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start]))) {
            ++start;
        }
        if (start >= text.size() || text[start] != '"') {
            return std::nullopt;
        }

        bool esc = false;
        for (std::size_t i = start + 1; i < text.size(); ++i) {
            const char ch = text[i];
            if (esc) {
                esc = false;
                continue;
            }
            if (ch == '\\') {
                esc = true;
                continue;
            }
            if (ch == '"') {
                return std::string(text.substr(start, i - start + 1));
            }
        }
        return std::nullopt;
    }

    inline nlohmann::json parseExecuteCodeText(std::string_view text) {
        constexpr std::string_view marker = "Return value JSON:";
        auto                       pos    = text.find(marker);
        if (pos == std::string_view::npos) {
            return nlohmann::json();
        }

        auto tail   = trimCopy(text.substr(pos + marker.size()));
        auto parsed = parseJsonMaybeNestedString(tail);
        if (!parsed.is_discarded()) {
            return parsed;
        }

        if (auto stringLiteral = extractJsonStringLiteralAtStart(tail)) {
            parsed = parseJsonMaybeNestedString(*stringLiteral);
            if (!parsed.is_discarded()) {
                return parsed;
            }
        }

        if (auto jsonText = extractFirstJsonText(tail)) {
            parsed = parseJsonMaybeNestedString(*jsonText);
            if (!parsed.is_discarded()) {
                return parsed;
            }
        }

        return nlohmann::json();
    }

    inline nlohmann::json parseFirstJsonFromDirtyText(std::string_view text) {
        auto executeCodeParsed = parseExecuteCodeText(text);
        if (!executeCodeParsed.is_null() && !executeCodeParsed.is_discarded()) {
            return executeCodeParsed;
        }

        auto jsonText = extractFirstJsonText(text);
        if (!jsonText) {
            return nlohmann::json{
                {"ok", false},
                {"error", {{"code", "NO_JSON_FOUND"}, {"message", "No JSON object or array found in IPC text"}}},
                {"raw_preview", std::string(text.substr(0, std::min<std::size_t>(text.size(), 500)))}
            };
        }

        auto parsed = parseJsonMaybeNestedString(*jsonText);
        if (parsed.is_discarded()) {
            return nlohmann::json{
                {"ok", false},
                {"error", {{"code", "JSON_PARSE_FAILED"}, {"message", "Failed to parse first JSON from IPC text"}}},
                {"json_preview", jsonText->substr(0, std::min<std::size_t>(jsonText->size(), 500))}
            };
        }
        return parsed;
    }

    inline std::string jsonScalarToString(const nlohmann::json& value) {
        if (value.is_string()) {
            return value.get<std::string>();
        }
        if (value.is_number_float()) {
            std::ostringstream out;
            out << value.get<double>();
            return out.str();
        }
        if (value.is_number_integer()) {
            return std::to_string(value.get<long long>());
        }
        if (value.is_number_unsigned()) {
            return std::to_string(value.get<unsigned long long>());
        }
        if (value.is_boolean()) {
            return value.get<bool>() ? "true" : "false";
        }
        if (value.is_null()) {
            return "";
        }
        return value.dump();
    }

    inline std::string htmlEscape(std::string_view value, bool escapeQuote = false) {
        std::string out;
        out.reserve(value.size() + 16);
        for (const char ch : value) {
            switch (ch) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += escapeQuote ? "&quot;" : "\"";
                break;
            default:
                out += ch;
                break;
            }
        }
        return out;
    }

    inline std::string htmlTagForType(std::string_view typeName) {
        if (typeName == "Button") {
            return "button";
        }
        if (typeName == "Label" || typeName == "RichText") {
            return "span";
        }
        return "div";
    }

    inline double jsonNumberOr(const nlohmann::json& object, std::string_view key, double fallback) {
        if (!object.is_object() || !object.contains(std::string(key))) {
            return fallback;
        }
        const auto& value = object[std::string(key)];
        return value.is_number() ? value.get<double>() : fallback;
    }

    inline std::string jsonStringOr(const nlohmann::json& object, std::string_view key, std::string fallback) {
        if (!object.is_object() || !object.contains(std::string(key))) {
            return fallback;
        }
        const auto& value = object[std::string(key)];
        return value.is_string() ? value.get<std::string>() : std::move(fallback);
    }

    inline bool jsonBoolOr(const nlohmann::json& object, std::string_view key, bool fallback) {
        if (!object.is_object() || !object.contains(std::string(key))) {
            return fallback;
        }
        const auto& value = object[std::string(key)];
        return value.is_boolean() ? value.get<bool>() : fallback;
    }

    inline std::string layoutExprToCss(const nlohmann::json& value, std::string_view axis) {
        if (!value.is_object()) {
            return {};
        }
        if (jsonBoolOr(value, "fit", false)) {
            return "100%";
        }

        const double absValue = jsonNumberOr(value, "absoluteValue", 0.0);
        const double relValue = jsonNumberOr(value, "relativeValue", 0.0);
        const auto   follow   = jsonStringOr(value, "followType", "none");

        std::ostringstream out;
        if (follow == "children") {
            if (absValue == 0.0 && relValue == 1.0) {
                return "fit-content";
            }
            out << "calc(fit-content * " << relValue << " + " << absValue << "px)";
            return out.str();
        }
        if (follow == "maxChildren") {
            if (absValue == 0.0 && relValue == 1.0) {
                return "max-content";
            }
            out << "calc(max-content * " << relValue << " + " << absValue << "px)";
            return out.str();
        }
        if (follow == "none" || relValue == 0.0) {
            out << absValue << "px";
            return out.str();
        }
        if (follow == "parent") {
            out << "calc(parent." << (axis == "x" ? "width" : "height") << " * " << relValue << " + " << absValue
                << "px)";
            return out.str();
        }
        out << "calc(" << follow << " * " << relValue << " + " << absValue << "px)";
        return out.str();
    }

    inline void addStyle(std::vector<std::pair<std::string, std::string>>& style, std::string key, std::string value) {
        if (!value.empty()) {
            style.emplace_back(std::move(key), std::move(value));
        }
    }

    inline std::string joinStyle(const std::vector<std::pair<std::string, std::string>>& style) {
        std::ostringstream out;
        bool               first = true;
        for (const auto& [key, value] : style) {
            if (!first) {
                out << ';';
            }
            first = false;
            out << key << ':' << value;
        }
        return out.str();
    }

    inline bool jsonArrayHasNumbers(const nlohmann::json& array, std::size_t count) {
        if (!array.is_array() || array.size() < count) {
            return false;
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (!array[i].is_number()) {
                return false;
            }
        }
        return true;
    }

    inline std::string jsonNumberPairAttr(const nlohmann::json& array) {
        std::ostringstream out;
        out << jsonScalarToString(array[0]) << ',' << jsonScalarToString(array[1]);
        return out.str();
    }

    inline std::string nodeHtmlAttrs(const nlohmann::json& node) {
        std::vector<std::string> attrs;
        attrs.push_back("data-type=\"" + htmlEscape(node.value("type", std::string()), true) + "\"");
        attrs.push_back("data-path=\"" + htmlEscape(node.value("path", std::string()), true) + "\"");

        if (node.contains("computed") && node["computed"].is_object()) {
            const auto& computed = node["computed"];
            const auto& size     = computed.contains("size") ? computed["size"] : nlohmann::json();
            const auto& pos      = computed.contains("position") ? computed["position"] : nlohmann::json();
            const auto& global   = computed.contains("global_position") ? computed["global_position"] : nlohmann::json();
            if (jsonArrayHasNumbers(size, 2)) {
                attrs.push_back("data-runtime-size=\"" + htmlEscape(jsonNumberPairAttr(size), true) + "\"");
            }
            if (jsonArrayHasNumbers(pos, 2)) {
                attrs.push_back("data-runtime-position=\"" + htmlEscape(jsonNumberPairAttr(pos), true) + "\"");
            }
            if (jsonArrayHasNumbers(global, 2)) {
                attrs.push_back("data-runtime-global-position=\"" + htmlEscape(jsonNumberPairAttr(global), true) + "\"");
            }
        }

        const auto& layout = node.contains("layout") ? node["layout"] : nlohmann::json();
        if (layout.is_object()) {
            if (layout.contains("anchor_from")) {
                attrs.push_back("data-anchor-from=\"" + htmlEscape(jsonScalarToString(layout["anchor_from"]), true) + "\"");
            }
            if (layout.contains("anchor_to")) {
                attrs.push_back("data-anchor-to=\"" + htmlEscape(jsonScalarToString(layout["anchor_to"]), true) + "\"");
            }
            if (layout.contains("size_x") && layout["size_x"].is_object()) {
                attrs.push_back(
                    "data-follow-x=\"" + htmlEscape(jsonStringOr(layout["size_x"], "followType", "none"), true) + "\""
                );
            }
            if (layout.contains("size_y") && layout["size_y"].is_object()) {
                attrs.push_back(
                    "data-follow-y=\"" + htmlEscape(jsonStringOr(layout["size_y"], "followType", "none"), true) + "\""
                );
            }
        }

        std::vector<std::pair<std::string, std::string>> style;
        if (layout.is_object()) {
            addStyle(style, "width", layoutExprToCss(layout.contains("size_x") ? layout["size_x"] : nlohmann::json(), "x"));
            addStyle(style, "height", layoutExprToCss(layout.contains("size_y") ? layout["size_y"] : nlohmann::json(), "y"));
            addStyle(
                style,
                "left",
                layoutExprToCss(layout.contains("position_x") ? layout["position_x"] : nlohmann::json(), "x")
            );
            addStyle(
                style,
                "top",
                layoutExprToCss(layout.contains("position_y") ? layout["position_y"] : nlohmann::json(), "y")
            );
        }

        if (style.empty() && node.contains("computed") && node["computed"].is_object()) {
            const auto& computed = node["computed"];
            const auto& size     = computed.contains("size") ? computed["size"] : nlohmann::json();
            const auto& pos      = computed.contains("position") ? computed["position"] : nlohmann::json();
            if (size.is_array() && size.size() >= 2) {
                addStyle(style, "width", jsonScalarToString(size[0]) + "px");
                addStyle(style, "height", jsonScalarToString(size[1]) + "px");
            }
            if (pos.is_array() && pos.size() >= 2) {
                addStyle(style, "left", jsonScalarToString(pos[0]) + "px");
                addStyle(style, "top", jsonScalarToString(pos[1]) + "px");
            }
        }

        if (!style.empty()) {
            attrs.push_back("style=\"" + htmlEscape(joinStyle(style), true) + "\"");
        }

        std::ostringstream out;
        for (std::size_t i = 0; i < attrs.size(); ++i) {
            if (i != 0) {
                out << ' ';
            }
            out << attrs[i];
        }
        return out.str();
    }

    inline std::string nodeText(const nlohmann::json& node) {
        if (!node.contains("text") || !node["text"].is_object()) {
            return {};
        }
        const auto& textObj = node["text"];
        if (!textObj.contains("text")) {
            return {};
        }
        return jsonScalarToString(textObj["text"]);
    }

    inline std::string treeToHtmlPseudo(const nlohmann::json& node, int indent = 0) {
        if (!node.is_object() || node.contains("error")) {
            return {};
        }

        const auto        tag      = htmlTagForType(node.value("type", std::string()));
        const std::string pad(static_cast<std::size_t>(indent) * 2, ' ');
        const auto        attrs    = nodeHtmlAttrs(node);
        const auto        text     = nodeText(node);
        const auto&       children = node.contains("children") ? node["children"] : nlohmann::json();

        if (!children.is_array() || children.empty()) {
            std::ostringstream line;
            line << pad << '<' << tag << ' ' << attrs << '>';
            if (!text.empty()) {
                line << htmlEscape(text);
            }
            line << "</" << tag << '>';
            return line.str();
        }

        std::ostringstream out;
        out << pad << '<' << tag << ' ' << attrs << '>';
        if (!text.empty()) {
            out << '\n' << pad << "  " << htmlEscape(text);
        }
        for (const auto& child : children) {
            auto childHtml = treeToHtmlPseudo(child, indent + 1);
            if (!childHtml.empty()) {
                out << '\n' << childHtml;
            }
        }
        out << '\n' << pad << "</" << tag << '>';
        return out.str();
    }

    inline double jsonArrayNumberOr(const nlohmann::json& array, std::size_t index, double fallback) {
        if (!array.is_array() || array.size() <= index || !array[index].is_number()) {
            return fallback;
        }
        return array[index].get<double>();
    }

    inline const char* svgColorForType(std::string_view typeName) {
        if (typeName == "Button") {
            return "#2f80ed";
        }
        if (typeName == "Label" || typeName == "RichText") {
            return "#27ae60";
        }
        if (typeName == "Image") {
            return "#9b51e0";
        }
        if (typeName == "StackPanel" || typeName == "Grid" || typeName == "StackGrid") {
            return "#f2994a";
        }
        if (typeName == "Factory" || typeName == "Custom") {
            return "#eb5757";
        }
        return "#4f4f4f";
    }

    struct SvgNodeRef {
        const nlohmann::json* node = nullptr;
        int                   depth = 0;
    };

    inline void collectSvgNodes(const nlohmann::json& node, std::vector<SvgNodeRef>& out, int depth = 0) {
        if (!node.is_object() || node.contains("error")) {
            return;
        }
        out.push_back({&node, depth});
        if (!node.contains("children") || !node["children"].is_array()) {
            return;
        }
        for (const auto& child : node["children"]) {
            collectSvgNodes(child, out, depth + 1);
        }
    }

    inline std::string renderLabelForNode(const nlohmann::json& node, std::string_view labelMode) {
        if (labelMode == "none") {
            return {};
        }
        if (labelMode == "type") {
            return node.value("type", std::string());
        }
        if (labelMode == "path-tail") {
            auto path = node.value("path", std::string());
            if (path.empty()) {
                return node.value("name", std::string());
            }
            std::replace(path.begin(), path.end(), '/', ' ');
            std::istringstream parts(path);
            std::vector<std::string> names;
            for (std::string item; parts >> item;) {
                names.push_back(std::move(item));
            }
            if (names.empty()) {
                return {};
            }
            const auto begin = names.size() > 3 ? names.size() - 3 : 0;
            std::ostringstream out;
            for (std::size_t i = begin; i < names.size(); ++i) {
                if (i != begin) {
                    out << '/';
                }
                out << names[i];
            }
            return out.str();
        }
        auto name = node.value("name", std::string());
        if (name.empty()) {
            name = node.value("type", std::string());
        }
        return name;
    }

    inline std::string svgTitleForNode(const nlohmann::json& node) {
        std::ostringstream out;
        out << node.value("type", std::string("Unknown")) << " ";
        out << node.value("path", std::string());
        return out.str();
    }

    inline std::string treeToSvgDiagram(const nlohmann::json& data) {
        if (!data.is_object() || !data.contains("tree") || !data["tree"].is_object()) {
            return {};
        }
        const auto& root     = data["tree"];
        const auto& computed = root.contains("computed") && root["computed"].is_object() ? root["computed"] : nlohmann::json();
        const auto& size     = computed.contains("size") ? computed["size"] : nlohmann::json();
        const auto& rootGp   = computed.contains("global_position") ? computed["global_position"] : nlohmann::json();
        const double width   = std::max(1.0, jsonArrayNumberOr(size, 0, 640.0));
        const double height  = std::max(1.0, jsonArrayNumberOr(size, 1, 360.0));
        const double originX = jsonArrayNumberOr(rootGp, 0, 0.0);
        const double originY = jsonArrayNumberOr(rootGp, 1, 0.0);
        const auto labelMode  = data.value("render_label", std::string("name"));
        const bool showLegend = data.value("render_legend", true);
        const double headerHeight = 18.0;
        const double legendHeight = showLegend ? 34.0 : 0.0;
        const double viewHeight   = height + headerHeight + legendHeight;

        std::vector<SvgNodeRef> nodes;
        collectSvgNodes(root, nodes);

        std::ostringstream svg;
        svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 " << width << ' ' << viewHeight
            << "\" width=\"" << width << "\" height=\"" << viewHeight << "\">\n";
        svg << "  <rect x=\"0\" y=\"0\" width=\"" << width << "\" height=\"" << viewHeight
            << "\" fill=\"#ffffff\"/>\n";
        svg << "  <text x=\"6\" y=\"14\" font-family=\"monospace\" font-size=\"10\" fill=\"#111827\">"
            << htmlEscape(data.value("screen", std::string()), false) << " runtime layout diagram</text>\n";
        svg << "  <g transform=\"translate(0 " << headerHeight << ")\">\n";
        svg << "    <rect x=\"0\" y=\"0\" width=\"" << width << "\" height=\"" << height
            << "\" fill=\"#111827\" opacity=\"0.08\"/>\n";

        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const auto& node = *nodes[i].node;
            const auto& comp = node.contains("computed") && node["computed"].is_object() ? node["computed"] : nlohmann::json();
            const auto& gp   = comp.contains("global_position") ? comp["global_position"] : nlohmann::json();
            const auto& ns   = comp.contains("size") ? comp["size"] : nlohmann::json();
            double       x    = jsonArrayNumberOr(gp, 0, originX) - originX;
            double       y    = jsonArrayNumberOr(gp, 1, originY) - originY;
            double       w    = jsonArrayNumberOr(ns, 0, 0.0);
            double       h    = jsonArrayNumberOr(ns, 1, 0.0);
            const auto type  = node.value("type", std::string());
            const bool visible = node.value("visible", false);
            const char* color = svgColorForType(type);
            const double opacity = visible ? 0.20 : 0.06;
            const double strokeOpacity = visible ? 0.95 : 0.35;
            const double inset = std::min(3.0, static_cast<double>(nodes[i].depth) * 0.65);

            if (w <= 0.5 || h <= 0.5) {
                const double cx = x;
                const double cy = y;
                svg << "    <g opacity=\"" << strokeOpacity << "\"><title>" << htmlEscape(svgTitleForNode(node))
                    << "</title><line x1=\"" << cx - 3 << "\" y1=\"" << cy
                    << "\" x2=\"" << cx + 3 << "\" y2=\"" << cy << "\" stroke=\"" << color
                    << "\" stroke-width=\"1\"/><line x1=\"" << cx << "\" y1=\"" << cy - 3 << "\" x2=\"" << cx
                    << "\" y2=\"" << cy + 3 << "\" stroke=\"" << color << "\" stroke-width=\"1\"/></g>\n";
                continue;
            }

            if (w > inset * 2.0 && h > inset * 2.0) {
                x += inset;
                y += inset;
                w -= inset * 2.0;
                h -= inset * 2.0;
            }

            svg << "    <rect x=\"" << x << "\" y=\"" << y << "\" width=\"" << w << "\" height=\"" << h
                << "\" fill=\"" << color << "\" fill-opacity=\"" << opacity << "\" stroke=\"" << color
                << "\" stroke-opacity=\"" << strokeOpacity << "\" stroke-width=\"" << (i == 0 ? 1.4 : 0.8)
                << "\"><title>" << htmlEscape(svgTitleForNode(node)) << "</title></rect>\n";

            const auto label = renderLabelForNode(node, labelMode);
            if (!label.empty() && w >= 24.0 && h >= 10.0) {
                const double labelWidth = std::min(w - 2.0, 8.0 + static_cast<double>(std::min<std::size_t>(label.size(), 42)) * 4.8);
                svg << "    <rect x=\"" << x + 1 << "\" y=\"" << y + 1 << "\" width=\"" << labelWidth
                    << "\" height=\"11\" fill=\"#ffffff\" fill-opacity=\"0.72\"/>\n";
                svg << "    <text x=\"" << x + 4 << "\" y=\"" << y + 10
                    << "\" font-family=\"monospace\" font-size=\"8\" fill=\"#111827\"><title>"
                    << htmlEscape(svgTitleForNode(node)) << "</title>"
                    << htmlEscape(label.substr(0, 42)) << "</text>\n";
            }
        }

        svg << "  </g>\n";
        if (showLegend) {
            svg << "  <g transform=\"translate(6 " << height + headerHeight + 16.0
                << ")\" font-family=\"monospace\" font-size=\"8\" fill=\"#111827\">\n";
            svg << "    <rect x=\"-3\" y=\"-10\" width=\"" << std::max(1.0, width - 12.0)
                << "\" height=\"28\" fill=\"#ffffff\" fill-opacity=\"0.72\"/>\n";
            svg << "    <text y=\"0\">Panel gray, Button blue, Label green, Image purple, Stack/Grid orange, Custom/Factory red</text>\n";
            svg << "    <text y=\"11\">Generated from Minecraft runtime layout data; not a screenshot and not source JSON UI.</text>\n";
            svg << "  </g>\n";
        }
        svg << "</svg>";
        return svg.str();
    }

    inline void attachHtmlPseudoIfRequested(std::string_view command, nlohmann::json& parsed) {
        const auto text = trimCopy(command);
        if (text.rfind("/html", 0) != 0 || !parsed.is_object() || !parsed.value("ok", false)) {
            return;
        }
        if (!parsed.contains("data") || !parsed["data"].is_object()) {
            return;
        }
        auto& data = parsed["data"];
        if (!data.contains("tree")) {
            return;
        }
        const bool htmlOnly = text.find("--html-only") != std::string::npos;
        data["html"] = treeToHtmlPseudo(data["tree"]);
        data["html_note"] =
            "HTML-like pseudo output derived from Minecraft runtime layout/render data. Use it as a layout reference "
            "only; it is not source JSON UI reconstruction and not browser-accurate HTML.";
        if (htmlOnly) {
            nlohmann::json summary = {
                {"screen", data.value("screen", std::string())},
                {"root", data.value("root", std::string())},
                {"max_depth", data.value("max_depth", 0)},
                {"max_nodes", data.value("max_nodes", 0)},
                {"returned_nodes", data.value("returned_nodes", 0)},
                {"scanned_nodes", data.value("scanned_nodes", data.value("visited_nodes", 0))},
                {"scan_limit", data.value("scan_limit", 0)},
                {"truncated", data.value("truncated", false)},
                {"truncated_reason", data.value("truncated_reason", nlohmann::json(nullptr))}
            };
            if (data["tree"].is_object()) {
                summary["tree_root"] = {
                    {"name", data["tree"].value("name", std::string())},
                    {"type", data["tree"].value("type", std::string())},
                    {"path", data["tree"].value("path", std::string())},
                    {"visible", data["tree"].value("visible", false)},
                    {"children_count", data["tree"].value("children_count", 0)}
                };
            }
            const auto html     = data["html"];
            const auto htmlNote = data["html_note"];
            data.clear();
            data["html"]      = html;
            data["html_note"] = htmlNote;
            data["summary"]   = std::move(summary);
        }
    }

    inline void attachSvgDiagramIfRequested(std::string_view command, nlohmann::json& parsed) {
        const auto text = trimCopy(command);
        if (text.rfind("/render", 0) != 0 || !parsed.is_object() || !parsed.value("ok", false)) {
            return;
        }
        if (!parsed.contains("data") || !parsed["data"].is_object()) {
            return;
        }
        auto& data = parsed["data"];
        if (!data.contains("tree")) {
            return;
        }

        const auto svg = treeToSvgDiagram(data);
        nlohmann::json summary = {
            {"screen", data.value("screen", std::string())},
            {"root", data.value("root", std::string())},
            {"max_depth", data.value("max_depth", 0)},
            {"max_nodes", data.value("max_nodes", 0)},
            {"returned_nodes", data.value("returned_nodes", 0)},
            {"scanned_nodes", data.value("scanned_nodes", data.value("visited_nodes", 0))},
            {"scan_limit", data.value("scan_limit", 0)},
            {"truncated", data.value("truncated", false)},
            {"truncated_reason", data.value("truncated_reason", nlohmann::json(nullptr))},
            {"label", data.value("render_label", std::string("name"))},
            {"legend", data.value("render_legend", true)}
        };
        if (data["tree"].is_object()) {
            summary["tree_root"] = {
                {"name", data["tree"].value("name", std::string())},
                {"type", data["tree"].value("type", std::string())},
                {"path", data["tree"].value("path", std::string())},
                {"visible", data["tree"].value("visible", false)},
                {"children_count", data["tree"].value("children_count", 0)}
            };
        }

        data.clear();
        data["svg"] = svg;
        data["svg_note"] =
            "SVG layout diagram for user visual inspection. It is generated from Minecraft runtime layout data; it is "
            "not a screenshot and not source JSON UI. AI should prefer structured JSON or /html --html-only unless it "
            "has strong image understanding.";
        data["summary"] = std::move(summary);
    }

} // namespace mcdk::jsonui_debugger
