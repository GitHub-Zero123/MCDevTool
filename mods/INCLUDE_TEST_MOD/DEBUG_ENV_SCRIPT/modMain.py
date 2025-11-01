# -*- coding: utf-8 -*-
from .QuModLibs.QuMod import *
from .QuModLibs.Util import QConstInit
import sys
lambda: "By Zero123"

REF = 0

class STD_OUT_WRAPPER(object):
    def __init__(self, baseIO):
        self.baseIO = baseIO

    def __getattr__(self, name):
        return getattr(self.baseIO, name)

    def write(self, text, **args):
        if isinstance(text, str) and text.strip():
            self.lastText = text
            return self.baseIO.write("[Python] " + str(text), **args)
        return self.baseIO.write(text, **args)

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

def RELOAD_MOD():
    import os
    import gui
    from common.utils import xupdate
    appdata = os.path.join(os.getenv("APPDATA"), "MinecraftPE_Netease")
    neteasePath = os.path.join(appdata, "games", "com.netease")
    behaviorPath = os.path.join(neteasePath, "behavior_packs")
    if xupdate.updata_all(behaviorPath):
        gui.set_left_corner_notify_msg("[Dev] Scripts reloaded successfully.")
    else:
        gui.set_left_corner_notify_msg("[Dev] No script updates found.")

def CLOnKeyPressInGame(args={}):
    if args["isDown"] != "0":
        return
    key = args["key"]
    if key == "82":
        RELOAD_MOD()

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