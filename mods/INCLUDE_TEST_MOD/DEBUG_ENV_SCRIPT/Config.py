# -*- coding: utf-8 -*-
import json
import os

try:
    _TEXT_TYPE = unicode
except NameError:
    _TEXT_TYPE = str


def _DECODE_ENV_TEXT(value):
    if isinstance(value, _TEXT_TYPE):
        return value
    for encoding in ("mbcs", "utf-8"):
        try:
            return value.decode(encoding)
        except (LookupError, UnicodeError):
            pass
    return value


def _GET_JSON_ENV(name, expected_type, default):
    value = os.getenv(name)
    if value is None:
        return default
    try:
        value = json.loads(_DECODE_ENV_TEXT(value))
    except (TypeError, ValueError):
        return default
    return value if isinstance(value, expected_type) else default


DEBUG_CONFIG = _GET_JSON_ENV("MCDEV_DEBUG_OPTIONS", dict, {})
TARGET_MOD_DIRS = _GET_JSON_ENV("MCDEV_TARGET_MOD_DIRS", list, [])


def _GET_LOG_PROTOCOL():
    try:
        value = int(os.getenv("MCDEV_LOG_PROTOCOL", "0"))
    except (TypeError, ValueError):
        return 0
    return value if value in (0, 1) else 0


LOG_PROTOCOL = _GET_LOG_PROTOCOL()


def GET_DEBUG_IPC_PORT():
    port = os.getenv("MCDEV_DEBUG_IPC_PORT")
    try:
        port = int(port)
    except (TypeError, ValueError):
        return None
    return port if 1 <= port <= 65535 else None
