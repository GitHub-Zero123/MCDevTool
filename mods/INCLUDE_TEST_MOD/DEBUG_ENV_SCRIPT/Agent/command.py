# -*- coding: utf-8 -*-
from .util import safe_text


class AgentCommand(object):
    def __init__(self, raw):
        self.raw = safe_text(raw or "/help").strip()
        self.parts = [p for p in self.raw.split() if p]

    def empty(self):
        return not self.parts

    def head(self):
        return self.parts[0] if self.parts else "/help"

    def tail(self, index=1):
        return self.parts[index:]

    @staticmethod
    def parse_options(parts):
        options = {}
        args = []
        for part in parts:
            if part.startswith("--"):
                body = part[2:]
                if "=" in body:
                    key, value = body.split("=", 1)
                    options[key.replace("-", "_")] = value
                else:
                    options[body.replace("-", "_")] = True
            else:
                args.append(part)
        return args, options


class CommandDispatcher(object):
    def __init__(self, runtime):
        self.runtime = runtime

    def dispatch(self, raw_cmd):
        cmd = AgentCommand(raw_cmd)
        if cmd.empty() or cmd.head() == "/help" or cmd.head().startswith("/help"):
            return self.runtime.help()
        if cmd.head() == "/observe":
            args, options = AgentCommand.parse_options(cmd.tail())
            return self.runtime.observe(options)
        if cmd.head() == "/task":
            return self.dispatch_task(cmd)
        return self.runtime.error("UNKNOWN_COMMAND", "Unknown game_agent command: " + cmd.raw)

    def dispatch_task(self, cmd):
        parts = cmd.tail()
        if not parts:
            return self.runtime.error("INVALID_COMMAND", "Missing /task subcommand. Use /help.")
        sub = parts[0]
        if sub == "start":
            if len(parts) < 2:
                return self.runtime.error("INVALID_COMMAND", "Missing task type. Use /help.")
            task_type = parts[1]
            if task_type == "run_command":
                prefix = "/task start run_command"
                command = cmd.raw[len(prefix):].strip()
                return self.runtime.start_task(task_type, [command], {})
            args, options = AgentCommand.parse_options(parts[2:])
            return self.runtime.start_task(task_type, args, options)
        if sub == "get" and len(parts) >= 2:
            return self.runtime.get_task(parts[1])
        if sub == "list":
            return self.runtime.list_tasks()
        if sub == "cancel" and len(parts) >= 2:
            return self.runtime.cancel_task(parts[1])
        if sub == "cancel-all":
            return self.runtime.cancel_all()
        return self.runtime.error("INVALID_COMMAND", "Unknown /task subcommand. Use /help.")
