# -*- coding: utf-8 -*-
import time

try:
    UNICODE_TYPE = unicode
except NameError:
    UNICODE_TYPE = str


def now_ms():
    return int(time.time() * 1000)


def safe_text(value):
    if isinstance(value, UNICODE_TYPE):
        return value
    try:
        return str(value)
    except Exception:
        return "<text failed>"


def json_safe(value, depth=0):
    if depth > 6:
        return safe_text(value)
    if value is None or isinstance(value, (bool, int, float)):
        return value
    if isinstance(value, UNICODE_TYPE):
        return value
    if isinstance(value, str):
        return value
    if isinstance(value, (list, tuple)):
        return [json_safe(v, depth + 1) for v in value]
    if isinstance(value, dict):
        out = {}
        for k, v in value.items():
            out[safe_text(k)] = json_safe(v, depth + 1)
        return out
    return {"__type__": type(value).__name__, "__repr__": safe_text(repr(value))}


def ok(data=None, **kwargs):
    result = {"ok": True}
    if data is not None:
        result["data"] = data
    result.update(kwargs)
    return result


def err(code, message, **kwargs):
    result = {"ok": False, "error": {"code": code, "message": message}}
    result.update(kwargs)
    return result


def as_bool(value, default=False):
    if value is None:
        return default
    if isinstance(value, bool):
        return value
    text = safe_text(value).lower()
    if text in ("1", "true", "yes", "on"):
        return True
    if text in ("0", "false", "no", "off"):
        return False
    return default


def as_int(value, default=0, minimum=None, maximum=None):
    try:
        out = int(value)
    except Exception:
        out = default
    if minimum is not None and out < minimum:
        out = minimum
    if maximum is not None and out > maximum:
        out = maximum
    return out


def as_float(value, default=0.0, minimum=None, maximum=None):
    try:
        out = float(value)
    except Exception:
        out = default
    if minimum is not None and out < minimum:
        out = minimum
    if maximum is not None and out > maximum:
        out = maximum
    return out
