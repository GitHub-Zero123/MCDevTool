# -*- coding: utf-8 -*-
from .QuModLibs.QuMod import *
from .Game import RELOAD_MOD, _RELOAD_MOD, RELOAD_ADDON, \
    RELOAD_WORLD, RELOAD_SHADERS
from .Config import DEBUG_CONFIG
import sys
lambda: "By Zero123"

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
    from . import IPCSystem
    def _DESTROY():
        global REF
        REF -= 1
        IPCSystem.ON_CLIENT_EXIT()
        if REF != 0:
            return
        REST_STDOUT()
    from .QuModLibs.Systems.Loader.Client import LoaderSystem
    LoaderSystem.REG_DESTROY_CALL_FUNC(_DESTROY)

    LoaderSystem.getSystem().nativeStaticListen(
        "OnKeyPressInGame",
        CLOnKeyPressInGame
    )
    IPCSystem.ON_CLIENT_INIT()

try:
    _RELOAD_MOD()
except:
    pass