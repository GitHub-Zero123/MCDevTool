# -*- coding: utf-8 -*-
from common.utils import xupdate
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