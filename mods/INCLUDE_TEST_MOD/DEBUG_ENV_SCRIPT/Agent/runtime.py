# -*- coding: utf-8 -*-
import traceback

from .command import CommandDispatcher
from .events import EventBridge, EventLog
from .tasks import TaskScheduler
from .util import as_bool, err, now_ms, ok
from .world import WorldProbe


HELP_TEXT = """game_agent commands:
/help
/observe [--radius=8] [--blocks] [--entities=false] [--inventory=false]
/task start wait [--ticks=20]
/task start run_command <command>
/task start walk_forward [--ticks=20]
/task start move [--forward=1.0] [--strafe=0.0] [--ticks=20]
/task start jump
/task start look --pitch=<pitch> --yaw=<yaw>
/task start look --x=<x> --y=<y> --z=<z>
/task start select_slot --slot=<0-8>
/task start use_item [--release-after=<ticks>]
/task start release_item
/task start use_block <x> <y> <z> [--face=1]
/task start attack_entity <entity_id>
/task get <task_id>
/task list
/task cancel <task_id>
/task cancel-all

Use /observe once to build the first decision baseline. After that, task results include observation_delta and recent events for follow-up decisions.

Mining, placement, and UI clicking task names are reserved by the runtime-agent design, but are not exposed as working tasks until their tick controllers are stable."""


class RuntimeAgent(object):
    def __init__(self):
        self.tick_count = 0
        self.world = WorldProbe()
        self.events = EventLog()
        self.scheduler = TaskScheduler(self)
        self.dispatcher = CommandDispatcher(self)
        self.bridge = EventBridge(self)

    def ensure_started(self):
        self.bridge.ensure_registered()

    def destroy(self):
        self.scheduler.cancel_all()

    def on_tick(self):
        self.tick_count += 1
        self.scheduler.update()

    def record_event(self, event_type, data=None):
        self.events.record(event_type, data or {}, self.tick_count, now_ms())

    def help(self):
        return ok({"help": HELP_TEXT})

    def error(self, code, message, **kwargs):
        return err(code, message, **kwargs)

    def observe(self, options=None):
        options = options or {}
        data = self.world.observe(
            radius=options.get("radius", 8),
            include_blocks=as_bool(options.get("blocks"), False),
            include_entities=as_bool(options.get("entities"), True),
            include_inventory=as_bool(options.get("inventory"), True),
            tasks=self.scheduler.running(),
            events=self.events.recent(),
        )
        return ok(data)

    def observation_delta(self):
        return self.world.observe(
            radius=6,
            include_blocks=False,
            include_entities=True,
            include_inventory=False,
            tasks=self.scheduler.running(),
            events=self.events.recent(),
        )

    def start_task(self, task_type, args=None, options=None):
        try:
            task, replaced = self.scheduler.start(task_type, args, options)
        except NotImplementedError as exc:
            return err("NOT_IMPLEMENTED", str(exc), supported_now=[
                "wait",
                "run_command",
                "walk_forward",
                "move",
                "jump",
                "look",
                "select_slot",
                "use_item",
                "release_item",
                "use_block",
                "attack_entity",
            ])
        except Exception as exc:
            return err("INVALID_ARGUMENT", str(exc))

        self.record_event("task_started", {"task_id": task.task_id, "task": task.task_type, "channel": task.channel})
        return ok({"started": task.to_json(), "replaced": replaced, "observation_delta": self.observation_delta()})

    def get_task(self, task_id):
        task = self.scheduler.get(task_id)
        if not task:
            return err("TASK_NOT_FOUND", "Task not found: " + task_id)
        data = task.to_json()
        data["observation_delta"] = self.observation_delta()
        return ok(data)

    def list_tasks(self):
        return ok({"tasks": self.scheduler.list()})

    def cancel_task(self, task_id):
        task = self.scheduler.cancel(task_id)
        if not task:
            return err("TASK_NOT_FOUND", "Task not found: " + task_id)
        return ok(task.to_json())

    def cancel_all(self):
        return ok({"cancelled": self.scheduler.cancel_all()})

    def handle_cmd(self, cmd):
        self.ensure_started()
        return self.dispatcher.dispatch(cmd)


_RUNTIME = None


def get_runtime():
    global _RUNTIME
    if _RUNTIME is None:
        _RUNTIME = RuntimeAgent()
    return _RUNTIME


def handle_cmd(cmd):
    try:
        return get_runtime().handle_cmd(cmd)
    except Exception as exc:
        return err("AGENT_EXCEPTION", str(exc), traceback=traceback.format_exc())


def destroy():
    global _RUNTIME
    if _RUNTIME is not None:
        _RUNTIME.destroy()
    _RUNTIME = None
