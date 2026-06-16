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
- window_size: client window size.)";
        }
        if (command == "overview" || command == "/overview") {
            return R"(/overview [--screen=top|all|<screen>] [--child-limit=12]
Return a compact starting point for AI UI inspection.

It lists current screens, probes common native JSON UI root candidates, returns direct child summaries, and suggests next commands. Use this before /tree or /html when you do not know the component path.)";
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
- --visible-only filters hidden nodes.)";
        }
        if (command == "html" || command == "/html") {
            return R"(/html <screen> <path> [--depth=2] [--max-nodes=80] [--visible-only] [--html-only]
Return the same bounded tree plus HTML-like pseudo output for AI layout reading.

The HTML-like output is derived from Minecraft runtime layout/render data. It is only a layout reference, not a source JSON UI reconstruction and not browser-accurate HTML.

Options:
- --html-only omits the full tree and returns html plus a compact summary.)";
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
/overview [--screen=top|all|<screen>] [--child-limit=12]
/probe <screen> <path>
/children <screen> <path> [--detail] [--limit=50]
/node <screen> <path> [--fields=basic,layout,text,container]
/tree <screen> <path> [--depth=2] [--max-nodes=80] [--visible-only]
/html <screen> <path> [--depth=2] [--max-nodes=80] [--visible-only] [--html-only]
/find <screen> <path> <query> [--type=Button] [--match=name] [--depth=5] [--limit=30]

Safety:
- Start with /overview when component paths are unknown.
- Tree commands expand level by level.
- Default /tree limits: depth=2, max-nodes=80.
- /html is a runtime layout reference, not source JSON UI reconstruction.
- Avoid raw recursive JSON UI APIs on large roots.)";
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

def _overview(screen_arg, child_limit):
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
        for path in ROOT_CANDIDATES:
            try:
                root = _probe_root_candidate(screen, path, child_limit)
                if root.get('ok'):
                    roots.append(root)
            except Exception:
                pass
        suggested_root = None
        if roots:
            visible_roots = [r for r in roots if r.get('visible')]
            suggested_root = (visible_roots or roots)[0].get('path')
        out_screens.append({
            'screen': screen,
            'is_top': screen == top_full,
            'roots': roots,
            'suggested_root': suggested_root,
            'suggested_next': [
                '/html %s %s --depth=2 --max-nodes=80 --visible-only --html-only' % (screen, suggested_root),
                '/find %s %s <query> --depth=5 --limit=30' % (screen, suggested_root),
                '/children %s %s --detail --limit=30' % (screen, suggested_root)
            ] if suggested_root else []
        })
    return {
        'top_screen': top_short,
        'top_screen_fullname': top_full,
        'screens': screens,
        'ui_size': _as_list(gui.get_client_ui_screen_size()),
        'window_size': _as_list(gui.get_client_screen_size()),
        'child_limit': child_limit,
        'screen_overviews': out_screens,
        'note': 'Use suggested_root as the starting component_path. Do not use / as a native JSON UI root.'
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
                names = gui.get_children_name_from_parent(screen, path)
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
            names = gui.get_children_name_from_parent(screen, path)
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
            names = gui.get_children_name_from_parent(screen, path)
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

def _run(cmd):
    args = _split_args(cmd)
    if not args or args[0] == '/help':
        return _ok('/help', {'text': 'Commands: /help, /screens, /overview, /probe, /children, /node, /tree, /html, /find'})
    name = args[0]
    if name == '/screens':
        return _ok(name, _screens())
    if name == '/overview':
        screen_arg = args[1] if len(args) >= 2 and not args[1].startswith('--') else _string_option(args[1:], 'screen', 'top')
        child_limit = _int_option(args[1:], 'child-limit', 12, 1, 50)
        return _ok(name, _overview(screen_arg, child_limit))
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
        depth = _int_option(args[3:], 'depth', 2, 0, 8)
        max_nodes = _int_option(args[3:], 'max-nodes', 80, 1, 500)
        visible_only = bool(_option(args[3:], 'visible-only', False))
        return _ok(name, _tree(args[1], args[2], depth, max_nodes, visible_only))
    if name == '/html':
        if len(args) < 3:
            return _err(name, 'usage: /html <screen> <path> [--depth=2] [--max-nodes=80]', 'USAGE')
        depth = _int_option(args[3:], 'depth', 2, 0, 6)
        max_nodes = _int_option(args[3:], 'max-nodes', 80, 1, 300)
        visible_only = bool(_option(args[3:], 'visible-only', False))
        data = _tree(args[1], args[2], depth, max_nodes, visible_only, True)
        return _ok(name, data)
    if name == '/find':
        if len(args) < 4:
            return _err(name, 'usage: /find <screen> <path> <query> [--type=Button] [--match=name] [--depth=5] [--limit=30]', 'USAGE')
        depth = _int_option(args[4:], 'depth', 5, 0, 8)
        max_nodes = _int_option(args[4:], 'max-nodes', 300, 1, 1000)
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

    inline std::string nodeHtmlAttrs(const nlohmann::json& node) {
        std::vector<std::string> attrs;
        attrs.push_back("data-type=\"" + htmlEscape(node.value("type", std::string()), true) + "\"");
        attrs.push_back("data-path=\"" + htmlEscape(node.value("path", std::string()), true) + "\"");

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

} // namespace mcdk::jsonui_debugger
