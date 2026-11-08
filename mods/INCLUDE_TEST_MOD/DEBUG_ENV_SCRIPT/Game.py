# -*- coding: utf-8 -*-
from common.utils import xupdate
import mod.client.extraClientApi as clientApi
from .Config import TARGET_MOD_DIRS

def _RELOAD_MOD():
    state = False
    for rootModDir in TARGET_MOD_DIRS:
        try:
            if xupdate.updata_all(rootModDir):
                state = True
        except Exception:
            import traceback
            traceback.print_exc()
    return state

def _MODULE_FILE(module, subDirs):
    import os
    name = getattr(module, "__file__", None)
    if not name:
        return None
    for subDir in subDirs:
        path = os.path.join(subDir, name.replace(".", "/") + ".py")
        if os.path.exists(path):
            return path
    return None

def RELOAD_CHANGED_SCRIPTS():
    # 与 xupdate.updata_all 相同的扫描，但逐个模块捕获异常，一个失败不影响其余模块。
    import os, sys, traceback
    xupdate.set_load_time()
    since = xupdate.LAST_UP_ALL_TIMESTAMP
    latest = since
    subDirs = []
    for root in TARGET_MOD_DIRS:
        if os.path.isdir(root):
            subDirs.extend(os.path.join(root, sub) for sub in os.listdir(root) if os.path.isdir(os.path.join(root, sub)))
    changed = []
    for name, module in list(sys.modules.items()):
        path = _MODULE_FILE(module, subDirs) if module else None
        if path:
            mtime = os.stat(path).st_mtime
            if mtime > since:
                changed.append((name, path))
                latest = max(latest, mtime)
    xupdate.LAST_UP_ALL_TIMESTAMP = latest
    updated = []
    failed = []
    for name, path in sorted(changed):
        try:
            xupdate.update(name)
            updated.append(name)
        except Exception as e:
            failed.append({"module": name, "file": path, "error": str(e), "traceback": traceback.format_exc()})
    return {"updated": updated, "failed": failed}

def INIT_RELOAD_TIME():
    return xupdate.set_load_time()

def SEND_CLIENT_MSG(msg):
    import gui
    print(msg)
    gui.set_left_corner_notify_msg(msg)

def RELOAD_MOD():
    import gui
    msg = "[Dev] Scripts reloaded successfully."
    if not _RELOAD_MOD():
        msg = "[Dev] No script updates found."
    SEND_CLIENT_MSG(msg)

def RELOAD_ONCE_MODULE(moduleName):
    return xupdate.update(moduleName)

def RELOAD_ADDON():
    import gui
    import clientlevel
    clientlevel.refresh_addons()
    SEND_CLIENT_MSG("[Dev] Add-ons reloaded successfully.")

def RELOAD_WORLD():
    import clientlevel
    clientlevel.restart_local_game()

def RELOAD_SHADERS():
    import gui
    if clientApi.ReloadAllShaders():
        SEND_CLIENT_MSG("[Dev] Shaders reloaded successfully.")
        return
    SEND_CLIENT_MSG("[Dev] No shader updates found.")