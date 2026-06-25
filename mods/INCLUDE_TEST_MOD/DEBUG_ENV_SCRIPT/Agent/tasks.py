# -*- coding: utf-8 -*-
import traceback

from .control import FACE_UP, GameControl
from .util import as_float, as_int, json_safe


class AgentTask(object):
    task_type = "task"
    channel = "default"

    def __init__(self, task_id, args=None, timeout_ticks=200):
        self.task_id = task_id
        self.args = args or {}
        self.timeout_ticks = timeout_ticks
        self.elapsed_ticks = 0
        self.status = "running"
        self.result = None
        self.error = None
        self.events = []

    def add_event(self, event_type, data=None):
        self.events.append({"tick": self.elapsed_ticks, "type": event_type, "data": json_safe(data or {})})
        if len(self.events) > 32:
            self.events = self.events[-32:]

    def tick(self, runtime):
        self.elapsed_ticks += 1
        if self.elapsed_ticks > self.timeout_ticks:
            self.fail("TIMEOUT", "Task timed out after %d ticks" % self.elapsed_ticks)

    def cancel(self, reason="cancelled"):
        if self.status != "running":
            return
        self.status = "cancelled"
        self.result = {"reason": reason}
        self.add_event("cancelled", self.result)

    def complete(self, result=None):
        self.status = "completed"
        self.result = json_safe(result or {})
        self.add_event("completed", self.result)

    def fail(self, code, message):
        self.status = "failed"
        self.error = {"code": code, "message": message}
        self.add_event("failed", self.error)

    def to_json(self, include_events=True):
        data = {
            "task_id": self.task_id,
            "task": self.task_type,
            "channel": self.channel,
            "status": self.status,
            "elapsed_ticks": self.elapsed_ticks,
        }
        if self.result is not None:
            data["result"] = self.result
        if self.error is not None:
            data["error"] = self.error
        if include_events:
            data["events"] = self.events[-12:]
        return data


class WaitTask(AgentTask):
    task_type = "wait"
    channel = "wait"

    def __init__(self, task_id, ticks=20):
        ticks = as_int(ticks, 20, 1, 20 * 60 * 5)
        AgentTask.__init__(self, task_id, {"ticks": ticks}, ticks + 1)
        self.target_ticks = ticks

    def tick(self, runtime):
        AgentTask.tick(self, runtime)
        if self.status == "running" and self.elapsed_ticks >= self.target_ticks:
            self.complete({"waited_ticks": self.elapsed_ticks})


class RunCommandTask(AgentTask):
    task_type = "run_command"
    channel = "command"

    def __init__(self, task_id, command):
        AgentTask.__init__(self, task_id, {"command": command}, 20)
        self.command = command
        self.ran = False

    def tick(self, runtime):
        AgentTask.tick(self, runtime)
        if self.status != "running":
            return
        if self.ran:
            self.complete({"command": self.command})
            return
        self.ran = True
        try:
            import clientlevel
            result = clientlevel.execute_command(self.command) if hasattr(clientlevel, "execute_command") else None
            self.add_event("command_executed", {"command": self.command, "result": result})
        except Exception as exc:
            self.fail("COMMAND_FAILED", str(exc))


class MoveInputTask(AgentTask):
    task_type = "move"
    channel = "movement"

    def __init__(self, task_id, ticks=20, forward=1.0, strafe=0.0, task_type="move"):
        ticks = as_int(ticks, 20, 1, 20 * 30)
        forward = as_float(forward, 1.0, -1.0, 1.0)
        strafe = as_float(strafe, 0.0, -1.0, 1.0)
        AgentTask.__init__(self, task_id, {"ticks": ticks, "forward": forward, "strafe": strafe}, ticks + 5)
        self.task_type = task_type
        self.target_ticks = ticks
        self.forward = forward
        self.strafe = strafe
        self.locked = False
        self.start_pos = None
        self.final_pos = None
        self.control = GameControl()

    def player_id(self):
        return self.control.player_id()

    def factory(self):
        return self.control.factory()

    def get_pos(self):
        try:
            pos = self.factory().CreatePos(self.player_id()).GetFootPos()
            if pos:
                return [pos[0], pos[1], pos[2]]
        except Exception:
            pass
        return None

    def lock_input(self):
        if self.locked:
            return
        self.control.lock_input(self.strafe, self.forward)
        self.locked = True
        self.add_event("movement_locked", {"input": [self.strafe, self.forward]})

    def unlock(self):
        if not self.locked:
            return
        try:
            self.control.unlock_input()
        except Exception:
            pass
        self.locked = False
        self.add_event("movement_unlocked", {})

    def cancel(self, reason="cancelled"):
        self.unlock()
        AgentTask.cancel(self, reason)

    def fail(self, code, message):
        self.unlock()
        AgentTask.fail(self, code, message)

    def complete(self, result=None):
        self.unlock()
        AgentTask.complete(self, result)

    def tick(self, runtime):
        AgentTask.tick(self, runtime)
        if self.status != "running":
            self.unlock()
            return
        if self.elapsed_ticks == 1:
            self.start_pos = self.get_pos()
            self.add_event("start_pos", {"pos": self.start_pos})
        try:
            self.lock_input()
        except Exception as exc:
            self.fail("MOVE_LOCK_FAILED", str(exc))
            return
        if self.elapsed_ticks >= self.target_ticks:
            self.final_pos = self.get_pos()
            self.complete({
                "walked_ticks": self.elapsed_ticks,
                "input": [self.strafe, self.forward],
                "start_pos": self.start_pos,
                "final_pos": self.final_pos,
            })


class LookTask(AgentTask):
    task_type = "look"
    channel = "look"

    def __init__(self, task_id, pitch=None, yaw=None, target=None):
        AgentTask.__init__(self, task_id, {"pitch": pitch, "yaw": yaw, "target": target}, 5)
        self.pitch = pitch
        self.yaw = yaw
        self.target = target
        self.done = False
        self.control = GameControl()

    def tick(self, runtime):
        AgentTask.tick(self, runtime)
        if self.status != "running":
            return
        try:
            before = self.control.player_rot()
            if self.target:
                rot = self.control.look_at(self.target)
            else:
                if self.pitch is None or self.yaw is None:
                    self.fail("INVALID_ARGUMENT", "look requires --pitch and --yaw, or --x --y --z")
                    return
                rot = self.control.set_rot(float(self.pitch), float(self.yaw))
            self.complete({"before": before, "after": rot})
        except Exception as exc:
            self.fail("LOOK_FAILED", str(exc))


class JumpTask(AgentTask):
    task_type = "jump"
    channel = "movement_action"

    def __init__(self, task_id):
        AgentTask.__init__(self, task_id, {}, 5)
        self.control = GameControl()

    def tick(self, runtime):
        AgentTask.tick(self, runtime)
        if self.status != "running":
            return
        try:
            self.complete(self.control.jump())
        except Exception as exc:
            self.fail("JUMP_FAILED", str(exc))


class SelectSlotTask(AgentTask):
    task_type = "select_slot"
    channel = "inventory"

    def __init__(self, task_id, slot=0):
        slot = as_int(slot, 0, 0, 8)
        AgentTask.__init__(self, task_id, {"slot": slot}, 5)
        self.slot = slot
        self.control = GameControl()

    def tick(self, runtime):
        AgentTask.tick(self, runtime)
        if self.status != "running":
            return
        try:
            before = self.control.selected_slot()
            result = self.control.select_slot(self.slot)
            after = self.control.selected_slot()
            result["before_slot"] = before
            result["after_slot"] = after
            result["held_item"] = self.control.carried_item()
            self.complete(result)
        except Exception as exc:
            self.fail("SELECT_SLOT_FAILED", str(exc))


class UseItemTask(AgentTask):
    task_type = "use_item"
    channel = "interaction"

    def __init__(self, task_id, release_after=0):
        release_after = as_int(release_after, 0, 0, 20 * 30)
        AgentTask.__init__(self, task_id, {"release_after": release_after}, release_after + 20)
        self.release_after = release_after
        self.used = False
        self.control = GameControl()
        self.use_result = None
        self.before_item = None

    def cancel(self, reason="cancelled"):
        if self.release_after > 0:
            try:
                self.control.release_item()
            except Exception:
                pass
        AgentTask.cancel(self, reason)

    def fail(self, code, message):
        if self.release_after > 0:
            try:
                self.control.release_item()
            except Exception:
                pass
        AgentTask.fail(self, code, message)

    def tick(self, runtime):
        AgentTask.tick(self, runtime)
        if self.status != "running":
            return
        try:
            if not self.used:
                self.before_item = self.control.carried_item()
                self.use_result = self.control.use_item()
                self.used = True
                self.add_event("item_used", self.use_result)
                if self.release_after <= 0:
                    self.use_result["before_item"] = self.before_item
                    self.use_result["after_item"] = self.control.carried_item()
                    self.complete(self.use_result)
                    return
            if self.release_after > 0 and self.elapsed_ticks >= self.release_after:
                release_result = self.control.release_item()
                self.complete({
                    "use": self.use_result,
                    "release": release_result,
                    "held_before": self.before_item,
                    "held_after": self.control.carried_item(),
                    "held_ticks": self.elapsed_ticks,
                })
        except Exception as exc:
            self.fail("USE_ITEM_FAILED", str(exc))


class ReleaseItemTask(AgentTask):
    task_type = "release_item"
    channel = "interaction"

    def __init__(self, task_id):
        AgentTask.__init__(self, task_id, {}, 5)
        self.control = GameControl()

    def tick(self, runtime):
        AgentTask.tick(self, runtime)
        if self.status != "running":
            return
        try:
            before_item = self.control.carried_item()
            result = self.control.release_item()
            result["held_before"] = before_item
            result["held_after"] = self.control.carried_item()
            self.complete(result)
        except Exception as exc:
            self.fail("RELEASE_ITEM_FAILED", str(exc))


class UseBlockTask(AgentTask):
    task_type = "use_block"
    channel = "interaction"

    def __init__(self, task_id, x=0, y=0, z=0, face=FACE_UP):
        AgentTask.__init__(self, task_id, {"pos": [int(x), int(y), int(z)], "face": int(face)}, 20)
        self.pos = [int(x), int(y), int(z)]
        self.face = int(face)
        self.aim_ticks = 0
        self.used = False
        self.control = GameControl()

    def tick(self, runtime):
        AgentTask.tick(self, runtime)
        if self.status != "running":
            return
        try:
            if self.aim_ticks < 3:
                target = (self.pos[0] + 0.5, self.pos[1] + 0.5, self.pos[2] + 0.5)
                rot = self.control.look_at(target)
                self.aim_ticks += 1
                if self.aim_ticks == 1:
                    self.add_event("aiming", {"rot": rot})
                return
            if not self.used:
                before_item = self.control.carried_item()
                result = self.control.use_item_on_block(self.pos, self.face)
                self.used = True
                result["before_item"] = before_item
                result["after_item"] = self.control.carried_item()
                self.complete(result)
        except Exception as exc:
            self.fail("USE_BLOCK_FAILED", str(exc))


class AttackEntityTask(AgentTask):
    task_type = "attack_entity"
    channel = "combat"

    def __init__(self, task_id, entity_id):
        AgentTask.__init__(self, task_id, {"entity_id": entity_id}, 20)
        self.entity_id = entity_id
        self.aim_ticks = 0
        self.control = GameControl()

    def tick(self, runtime):
        AgentTask.tick(self, runtime)
        if self.status != "running":
            return
        try:
            entity_pos = runtime.world.entity_pos(self.entity_id)
            if entity_pos and self.aim_ticks < 3:
                self.control.look_at((entity_pos[0], entity_pos[1] + 0.8, entity_pos[2]))
                self.aim_ticks += 1
                return
            self.complete(self.control.attack_entity(self.entity_id))
        except Exception as exc:
            self.fail("ATTACK_FAILED", str(exc))


class TaskFactory(object):
    def create(self, task_id, task_type, args, options):
        if task_type == "wait":
            return WaitTask(task_id, options.get("ticks", 20))
        if task_type == "run_command":
            command = " ".join(args)
            if not command:
                raise ValueError("run_command requires a command string")
            return RunCommandTask(task_id, command)
        if task_type == "walk_forward" or task_type == "move_forward":
            return MoveInputTask(task_id, options.get("ticks", 20), 1.0, 0.0, "walk_forward")
        if task_type == "move":
            return MoveInputTask(
                task_id,
                options.get("ticks", 20),
                options.get("forward", 1.0),
                options.get("strafe", 0.0),
                "move",
            )
        if task_type == "jump":
            return JumpTask(task_id)
        if task_type == "look":
            target = None
            if "x" in options and "y" in options and "z" in options:
                target = (float(options["x"]), float(options["y"]), float(options["z"]))
            return LookTask(task_id, options.get("pitch"), options.get("yaw"), target)
        if task_type == "select_slot":
            slot = options.get("slot", args[0] if args else 0)
            return SelectSlotTask(task_id, slot)
        if task_type == "use_item":
            return UseItemTask(task_id, options.get("release_after", 0))
        if task_type == "release_item":
            return ReleaseItemTask(task_id)
        if task_type == "use_block":
            x = options.get("x", args[0] if len(args) > 0 else 0)
            y = options.get("y", args[1] if len(args) > 1 else 0)
            z = options.get("z", args[2] if len(args) > 2 else 0)
            face = options.get("face", FACE_UP)
            return UseBlockTask(task_id, x, y, z, face)
        if task_type == "attack_entity":
            entity_id = options.get("entity_id", args[0] if args else "")
            if not entity_id:
                raise ValueError("attack_entity requires an entity id")
            return AttackEntityTask(task_id, entity_id)
        raise NotImplementedError("Task '%s' is reserved but not implemented yet." % task_type)


class TaskScheduler(object):
    def __init__(self, runtime):
        self.runtime = runtime
        self.factory = TaskFactory()
        self.tasks = {}
        self.channels = {}
        self.next_task_id = 1

    def alloc_task_id(self):
        task_id = "task_%d" % self.next_task_id
        self.next_task_id += 1
        return task_id

    def start(self, task_type, args=None, options=None):
        args = args or []
        options = options or {}
        task_id = self.alloc_task_id()
        task = self.factory.create(task_id, task_type, args, options)
        replaced = None
        old_id = self.channels.get(task.channel)
        if old_id and old_id in self.tasks:
            old = self.tasks[old_id]
            old.cancel("replaced by %s" % task_id)
            replaced = old.to_json()

        self.tasks[task_id] = task
        self.channels[task.channel] = task_id
        task.add_event("started", task.args)
        return task, replaced

    def update(self):
        for task_id in list(self.tasks.keys()):
            task = self.tasks.get(task_id)
            if not task or task.status != "running":
                continue
            try:
                task.tick(self.runtime)
            except Exception as exc:
                task.fail("TICK_EXCEPTION", str(exc))
                task.add_event("traceback", {"traceback": traceback.format_exc()})
            if task.status != "running":
                try:
                    task.add_event("observation_delta", self.runtime.observation_delta())
                except Exception:
                    task.add_event("observation_delta_failed", {"traceback": traceback.format_exc()})
            if task.status != "running" and self.channels.get(task.channel) == task.task_id:
                del self.channels[task.channel]

    def get(self, task_id):
        return self.tasks.get(task_id)

    def list(self):
        return [task.to_json(False) for task in self.tasks.values()]

    def running(self):
        return [task.to_json(False) for task in self.tasks.values() if task.status == "running"]

    def cancel(self, task_id, reason="requested"):
        task = self.tasks.get(task_id)
        if task:
            task.cancel(reason)
            if self.channels.get(task.channel) == task.task_id:
                del self.channels[task.channel]
        return task

    def cancel_all(self):
        cancelled = []
        for task in self.tasks.values():
            if task.status == "running":
                task.cancel("cancel-all")
                cancelled.append(task.to_json(False))
        self.channels = {}
        return cancelled
