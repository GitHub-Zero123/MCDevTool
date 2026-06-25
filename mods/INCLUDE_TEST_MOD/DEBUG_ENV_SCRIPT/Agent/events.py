# -*- coding: utf-8 -*-
import traceback


class EventBridge(object):
    def __init__(self, runtime):
        self.runtime = runtime
        self.registered = False

    def ensure_registered(self):
        if self.registered:
            return
        try:
            from ..QuModLibs.Systems.Loader.Client import LoaderSystem
            system = LoaderSystem.getSystem()
            system.nativeStaticListen("OnScriptTickClient", self.on_client_tick)
            system.nativeStaticListen("OnKeyPressInGame", self.on_key_press)
            LoaderSystem.REG_DESTROY_CALL_FUNC(self.runtime.destroy)
            self.registered = True
        except Exception:
            traceback.print_exc()

    def on_client_tick(self, args=None):
        self.runtime.on_tick()

    def on_key_press(self, args=None):
        self.runtime.record_event("OnKeyPressInGame", args or {})


class EventLog(object):
    def __init__(self, max_count=80):
        self.max_count = max_count
        self.items = []

    def record(self, event_type, data=None, tick=0, time_ms=0):
        self.items.append({"tick": tick, "time_ms": time_ms, "type": event_type, "data": data or {}})
        if len(self.items) > self.max_count:
            self.items = self.items[-self.max_count:]

    def recent(self, count=12):
        return self.items[-count:]
