#pragma once

#include <string>

namespace mcdk::jsonui_reload_support {

    inline constexpr const char* StateAttr = "_mcdk_jsonui_reload_state";

    inline std::string buildPreparePreserveModUiPythonCode() {
        return R"PY(
import json
import sys

STATE_ATTR = '_mcdk_jsonui_reload_state'

def _state():
    state = getattr(sys, STATE_ATTR, None)
    if not isinstance(state, dict):
        state = {}
        setattr(sys, STATE_ATTR, state)
    state['version'] = 1
    return state

def _safe_call(func, default=None):
    try:
        return func()
    except Exception:
        return default

def _param_from_node(node, hud_fallback):
    # No monkey patch and no broad object inspection here. If a future UI wrapper
    # explicitly stores its creation dict here, reuse it.
    value = getattr(node, '_mcdk_reload_param', None)
    if isinstance(value, dict):
        return value
    if not hud_fallback:
        return None
    # create_ui entries preserved here are HUD-mounted UI nodes. Without the
    # original dict ScreenNode defaults input_mode to 1, which steals movement
    # input after restore. Use the smallest safe HUD fallback.
    return {'isHud': True, 'inputMode': 0}

def _is_builtin_ui_class(class_path):
    if not class_path:
        return True
    # NetEase built-in ModSDK UI classes normally live under mod.* or client.ui.*.
    # Preserve only user addon classes; do not revive built-in HUD helpers/pet/emote/etc.
    return class_path.startswith('mod.') or class_path.startswith('client.ui.')

def _collect_user_screen_defs(mgr):
    out = {}
    screen_def = getattr(mgr, 'screen_def', {}) or {}
    for key, ui_def in screen_def.items():
        try:
            class_path = ui_def[0] if len(ui_def) > 0 else ''
            if not _is_builtin_ui_class(class_path):
                out[key] = ui_def
        except Exception:
            pass
    return out

def _restore_user_screen_defs(mgr):
    state = _state()
    user_screen_defs = state.get('user_screen_defs', {})
    if not isinstance(user_screen_defs, dict):
        return 0
    screen_def = getattr(mgr, 'screen_def', None)
    if not isinstance(screen_def, dict):
        mgr.screen_def = {}
        screen_def = mgr.screen_def
    restored = 0
    for key, ui_def in user_screen_defs.items():
        if isinstance(ui_def, list) and len(ui_def) == 4:
            screen_def[key] = ui_def
            restored += 1
    return restored

def _make_create_record(mgr, node):
    def_key = _safe_call(lambda: node.get_def_key(), '')
    org_key = _safe_call(lambda: node.get_org_key(), '')
    if def_key == 'NETEASE_FORBUTTONCANCEL' or org_key == 'NETEASE_FORBUTTONCANCEL':
        return None
    ui_def = getattr(mgr, 'screen_def', {}).get(def_key)
    if not ui_def:
        return None
    class_path = ui_def[0] if len(ui_def) > 0 else ''
    if _is_builtin_ui_class(class_path):
        return None
    return {
        'kind': 'create_ui',
        'ui_def': ui_def,
        'param': _param_from_node(node, True),
        'def_key': def_key,
        'org_key': org_key,
    }

def _make_push_record(mgr, node):
    org_key = _safe_call(lambda: node.get_org_key(), '')
    if not org_key or ':' not in org_key:
        return None
    ui_def = getattr(mgr, 'screen_def', {}).get(org_key)
    if not ui_def:
        return None
    class_path = ui_def[0] if len(ui_def) > 0 else ''
    if _is_builtin_ui_class(class_path):
        return None
    return {
        'kind': 'push_screen',
        'ui_def': ui_def,
        'param': _param_from_node(node, False),
        'org_key': org_key,
    }

def _restore_push_record(mgr, rec):
    import gui
    import common.gameConfig as modGameCfg
    ui_def = rec.get('ui_def')
    if not isinstance(ui_def, list) or len(ui_def) != 4:
        return None
    child_fullname = ui_def[1]
    if mgr.get_screen(child_fullname):
        return mgr.get_screen(child_fullname)
    if not gui.push_screen(child_fullname, False):
        return None
    namespace, screen_name = mgr.part_fullname(child_fullname)
    ui_cls = mgr.get_cls_by_clsstr(ui_def[0])
    if not ui_cls:
        return None
    param = rec.get('param')
    if isinstance(param, dict):
        param['__isCustomParam__'] = True
    node = ui_cls(namespace, screen_name, param)
    node.set_def_key(ui_def[2])
    node.set_org_key(ui_def[3])
    node.screen_name = child_fullname
    node.is_push_screen = True
    node.touch_with_mouse = gui.is_touch_with_mouse()
    if getattr(mgr, '_ui_screen_stack', None):
        mgr._ui_screen_stack[-1].OnDeactive()
    mgr._ui_screen_stack.append(node)
    if getattr(mgr, 'mIsPC', False):
        gui.new_show_mouse(True)
    elif not node.touch_with_mouse:
        if not modGameCfg.B_EDITOR:
            gui.simulate_touch_with_mouse(True)
    return node

def _restore_records():
    from client.ui import uiManager
    mgr = uiManager.instance()
    state = _state()
    records = list(state.get('records', []))
    result = {'attempted': 0, 'created': 0, 'failed': []}
    try:
        mgr.uninit()
    except Exception:
        pass
    try:
        mgr.init()
    except Exception as exc:
        result['failed'].append({'stage': 'init', 'error': repr(exc)})
        state['last_restore'] = result
        return result
    result['screen_defs_restored'] = _restore_user_screen_defs(mgr)
    for rec in records:
        result['attempted'] += 1
        try:
            node = None
            if rec.get('kind') == 'create_ui':
                node = mgr.create_ui(rec.get('ui_def'), rec.get('param'))
            elif rec.get('kind') == 'push_screen':
                node = _restore_push_record(mgr, rec)
            if node is not None:
                result['created'] += 1
            else:
                result['failed'].append({'record': rec.get('kind'), 'key': rec.get('org_key') or rec.get('def_key'), 'error': 'returned None'})
        except Exception as exc:
            result['failed'].append({'record': rec.get('kind'), 'key': rec.get('org_key') or rec.get('def_key'), 'error': repr(exc)})
    try:
        mgr.set_input_mode(0)
    except Exception:
        pass
    try:
        import gui
        gui.set_focus_forbit(0)
    except Exception:
        pass
    state['last_restore'] = result
    return result

class _McdkUiReloadAfter(object):
    def UIDefReloadSceneStackAfter(self, args):
        try:
            from common import eventUtil
            eventUtil.instance.UnListenForEngineClient('UIDefReloadSceneStackAfter', self, self.UIDefReloadSceneStackAfter)
        except Exception:
            pass
        try:
            _restore_records()
        except Exception as exc:
            _state()['last_restore'] = {'attempted': 0, 'created': 0, 'failed': [{'stage': 'restore', 'error': repr(exc)}]}

try:
    from client.ui import uiManager
    from common import eventUtil
    mgr = uiManager.instance()
    records = []
    user_screen_defs = _collect_user_screen_defs(mgr)
    for node in list(getattr(mgr, '_ui_stack', []) or []):
        rec = _make_create_record(mgr, node)
        if rec:
            records.append(rec)
    for node in list((getattr(mgr, '_ui_bind', {}) or {}).values()):
        rec = _make_create_record(mgr, node)
        if rec:
            records.append(rec)
    for node in list(getattr(mgr, '_ui_screen_stack', []) or [])[1:]:
        rec = _make_push_record(mgr, node)
        if rec:
            records.append(rec)
    state = _state()
    listener = _McdkUiReloadAfter()
    state['records'] = records
    state['user_screen_defs'] = user_screen_defs
    state['listener'] = listener
    state['last_prepare'] = {'records': len(records), 'user_screen_defs': len(user_screen_defs)}
    state.pop('last_restore', None)
    eventUtil.instance.ListenForEngineClient('UIDefReloadSceneStackAfter', listener, listener.UIDefReloadSceneStackAfter)

    clear_stack_ok = _safe_call(lambda: mgr.clear_stack(), None)
    uninit_ok = True
    try:
        mgr.uninit()
    except Exception as exc:
        uninit_ok = False
        state['last_prepare']['uninit_error'] = repr(exc)

    _result = json.dumps({
        'ok': True,
        'state_attr': STATE_ATTR,
        'records': len(records),
        'user_screen_defs': len(user_screen_defs),
        'clear_stack_result': clear_stack_ok,
        'uninit_ok': uninit_ok,
        'note': 'Frozen a temporary ModSDK user UI snapshot. Ctrl+R must be triggered after this returns; restore runs after UIDefReloadSceneStackAfter.',
    }, ensure_ascii=False)
except Exception as exc:
    import traceback
    _result = json.dumps({
        'ok': False,
        'state_attr': STATE_ATTR,
        'error': repr(exc),
        'trace': traceback.format_exc(),
    }, ensure_ascii=False)
)PY";
    }

    inline std::string buildRestorePreservedModUiPythonCode() {
        return R"PY(
import json
import sys

STATE_ATTR = '_mcdk_jsonui_reload_state'

def _state():
    state = getattr(sys, STATE_ATTR, None)
    if not isinstance(state, dict):
        state = {}
        setattr(sys, STATE_ATTR, state)
    return state

def _restore_records():
    from client.ui import uiManager
    mgr = uiManager.instance()
    state = _state()
    records = list(state.get('records', []))
    result = {'attempted': 0, 'created': 0, 'failed': []}
    try:
        mgr.uninit()
    except Exception:
        pass
    try:
        mgr.init()
    except Exception as exc:
        result['failed'].append({'stage': 'init', 'error': repr(exc)})
        state['last_restore'] = result
        return result
    user_screen_defs = state.get('user_screen_defs', {})
    if not isinstance(getattr(mgr, 'screen_def', None), dict):
        mgr.screen_def = {}
    restored_defs = 0
    if isinstance(user_screen_defs, dict):
        for key, ui_def in user_screen_defs.items():
            if isinstance(ui_def, list) and len(ui_def) == 4:
                mgr.screen_def[key] = ui_def
                restored_defs += 1
    result['screen_defs_restored'] = restored_defs
    for rec in records:
        result['attempted'] += 1
        try:
            node = None
            if rec.get('kind') == 'create_ui':
                node = mgr.create_ui(rec.get('ui_def'), rec.get('param'))
            elif rec.get('kind') == 'push_screen':
                import gui
                import common.gameConfig as modGameCfg
                ui_def = rec.get('ui_def')
                if isinstance(ui_def, list) and len(ui_def) == 4:
                    child_fullname = ui_def[1]
                    if mgr.get_screen(child_fullname):
                        node = mgr.get_screen(child_fullname)
                    elif gui.push_screen(child_fullname, False):
                        namespace, screen_name = mgr.part_fullname(child_fullname)
                        ui_cls = mgr.get_cls_by_clsstr(ui_def[0])
                        if ui_cls:
                            param = rec.get('param')
                            if isinstance(param, dict):
                                param['__isCustomParam__'] = True
                            node = ui_cls(namespace, screen_name, param)
                            node.set_def_key(ui_def[2])
                            node.set_org_key(ui_def[3])
                            node.screen_name = child_fullname
                            node.is_push_screen = True
                            node.touch_with_mouse = gui.is_touch_with_mouse()
                            if getattr(mgr, '_ui_screen_stack', None):
                                mgr._ui_screen_stack[-1].OnDeactive()
                            mgr._ui_screen_stack.append(node)
                            if getattr(mgr, 'mIsPC', False):
                                gui.new_show_mouse(True)
                            elif not node.touch_with_mouse:
                                if not modGameCfg.B_EDITOR:
                                    gui.simulate_touch_with_mouse(True)
            if node is not None:
                result['created'] += 1
            else:
                result['failed'].append({'record': rec.get('kind'), 'key': rec.get('org_key') or rec.get('def_key'), 'error': 'returned None'})
        except Exception as exc:
            result['failed'].append({'record': rec.get('kind'), 'key': rec.get('org_key') or rec.get('def_key'), 'error': repr(exc)})
    try:
        mgr.set_input_mode(0)
    except Exception:
        pass
    try:
        import gui
        gui.set_focus_forbit(0)
    except Exception:
        pass
    state['last_restore'] = result
    return result

try:
    state = _state()
    listener = state.get('listener')
    if listener is not None:
        try:
            from common import eventUtil
            eventUtil.instance.UnListenForEngineClient('UIDefReloadSceneStackAfter', listener, listener.UIDefReloadSceneStackAfter)
        except Exception:
            pass
        state['listener'] = None
    result = _restore_records()
    _result = json.dumps({'ok': True, 'state_attr': STATE_ATTR, 'restore': result}, ensure_ascii=False)
except Exception as exc:
    import traceback
    _result = json.dumps({'ok': False, 'state_attr': STATE_ATTR, 'error': repr(exc), 'trace': traceback.format_exc()}, ensure_ascii=False)
)PY";
    }

} // namespace mcdk::jsonui_reload_support
