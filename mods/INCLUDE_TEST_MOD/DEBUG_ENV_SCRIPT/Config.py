# -*- coding: utf-8 -*-
from json import loads
_DEBUG_INFO = "{#debug_options}"
_TARGET_MOD_DIRS = "{#target_mod_dirs}"

try:
    DEBUG_CONFIG = loads(_DEBUG_INFO) if not isinstance(_DEBUG_INFO, dict) else _DEBUG_INFO
except:
    DEBUG_CONFIG = {}

try:
    TARGET_MOD_DIRS = loads(_TARGET_MOD_DIRS) if not isinstance(_TARGET_MOD_DIRS, list) else _TARGET_MOD_DIRS
except:
    TARGET_MOD_DIRS = []

def GET_DEBUG_IPC_PORT():
    import os
    port = os.getenv("MCDEV_DEBUG_IPC_PORT")
    if port is None:
        return None
    return int(port)