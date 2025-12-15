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

def RELOAD_MOD():
    import gui
    msg = "[Dev] Scripts reloaded successfully."
    if not _RELOAD_MOD():
        msg = "[Dev] No script updates found."
    gui.set_left_corner_notify_msg(msg)
    print(msg)

def RELOAD_ADDON():
    import gui
    import clientlevel
    clientlevel.refresh_addons()
    gui.set_left_corner_notify_msg("[Dev] Add-ons reloaded successfully.")

def RELOAD_WORLD():
    import clientlevel
    clientlevel.restart_local_game()

def RELOAD_SHADERS():
    import gui
    if clientApi.ReloadAllShaders():
        gui.set_left_corner_notify_msg("[Dev] Shaders reloaded successfully.")
        return
    gui.set_left_corner_notify_msg("[Dev] No shader updates found.")