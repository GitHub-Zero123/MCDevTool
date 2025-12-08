# -*- coding: utf-8 -*-
from .QuModLibs.QuMod import *
from .QuModLibs.Util import QConstInit
from common.utils import xupdate
from json import loads
import sys
lambda: "By Zero123"

_DEBUG_INFO = "{#debug_options}"
try:
    DEBUG_CONFIG = loads(_DEBUG_INFO) if not isinstance(_DEBUG_INFO, dict) else _DEBUG_INFO
except:
    DEBUG_CONFIG = {}

REF = 0

class STD_OUT_WRAPPER(object):
    def __init__(self, baseIO):
        self.baseIO = baseIO
        self._buffer = []

    def __getattr__(self, name):
        return getattr(self.baseIO, name)

    def write(self, text):
        self._buffer.append(text)
        buf = "".join(self._buffer)

        if "\n" not in buf:
            return

        lines = buf.split("\n")
        self._buffer = [lines.pop()]

        for line in lines:
            if line.strip() == "":
                self.baseIO.write("\n")
            else:
                self.baseIO.write("[Python] " + line + "\n")

    # def write(self, text, **args):
    #     if text in ("\n", "\r\n", "", " "):
    #         return self.baseIO.write(str(text), **args)
    #     return self.baseIO.write("[Python] " + str(text), **args)

    def close(self):
        return self.baseIO.close()

    def writelines(self, lines):
        for line in lines:
            self.write(line)

    def fileno(self):
        return self.baseIO.fileno()

stdout = sys.stdout
stderr = sys.stderr

def REST_STDOUT():
    sys.stdout = stdout
    sys.stderr = stderr

@QConstInit
def INIT():
    sys.stdout = STD_OUT_WRAPPER(sys.stdout)
    sys.stderr = STD_OUT_WRAPPER(sys.stderr)

@PRE_SERVER_LOADER_HOOK
def SERVER_INIT():
    global REF
    REF += 1
    def _DESTROY():
        global REF
        REF -= 1
        if REF != 0:
            return
        REST_STDOUT()
    from .QuModLibs.Systems.Loader.Server import LoaderSystem
    LoaderSystem.REG_DESTROY_CALL_FUNC(_DESTROY)

def _RELOAD_MOD():
    import os
    appdata = os.path.join(os.getenv("APPDATA"), "MinecraftPE_Netease")
    neteasePath = os.path.join(appdata, "games", "com.netease")
    behaviorPath = os.path.join(neteasePath, "behavior_packs")
    return xupdate.updata_all(behaviorPath)

def RELOAD_MOD():
    import gui
    if _RELOAD_MOD():
        gui.set_left_corner_notify_msg("[Dev] Scripts reloaded successfully.")
    else:
        gui.set_left_corner_notify_msg("[Dev] No script updates found.")

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

def CLOnKeyPressInGame(args={}):
    if args["isDown"] != "0":
        return
    if args["screenName"] != "hud_screen" and not DEBUG_CONFIG.get("reload_key_global", False):
        return
    key = args["key"]
    if key == str(DEBUG_CONFIG.get("reload_key", "82")):
        RELOAD_MOD()
    elif key == str(DEBUG_CONFIG.get("reload_world_key", "")):
        RELOAD_WORLD()
    elif key == str(DEBUG_CONFIG.get("reload_addon_key", "")):
        RELOAD_ADDON()
    elif key == str(DEBUG_CONFIG.get("reload_shaders_key", "")):
        RELOAD_SHADERS()

@PRE_CLIENT_LOADER_HOOK
def CLIENT_INIT():
    global REF
    REF += 1
    def _DESTROY():
        global REF
        REF -= 1
        if REF != 0:
            return
        REST_STDOUT()
    from .QuModLibs.Systems.Loader.Client import LoaderSystem
    LoaderSystem.REG_DESTROY_CALL_FUNC(_DESTROY)

    LoaderSystem.getSystem().nativeStaticListen(
        "OnKeyPressInGame",
        CLOnKeyPressInGame
    )

try:
    _RELOAD_MOD()
except:
    pass