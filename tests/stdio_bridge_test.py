#!/usr/bin/env python3
"""Verify the stdio proxy against a local fake MCP backend, without launching Minecraft."""

import json
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


IMAGE = {"type": "image", "mimeType": "image/jpeg", "data": "YWJj" * 262144}
RESULT = {
    "isError": False,
    "content": [IMAGE, {"type": "text", "text": "截图后的日志"}],
    "structuredContent": {"ok": True, "data": {"logs": ["测试日志"]}},
}
FAILURE = {
    "isError": True,
    "content": [{"type": "text", "text": "backend failure"}],
    "structuredContent": {"ok": False, "error": {"code": "FOCUS_LOST"}},
}


class Backend(BaseHTTPRequestHandler):
    calls = []
    offline = False

    def log_message(self, *_args):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if self.offline:
            self.send_response(503)
            self.end_headers()
            return
        response = {"jsonrpc": "2.0", "id": request.get("id"), "result": {}}
        if request["method"] == "tools/call":
            params = request["params"]
            self.calls.append(params)
            mode = params["arguments"].get("op")
            response["result"] = FAILURE if mode == "/failure" else RESULT
            if mode == "/missing":
                response.pop("result")
                response["error"] = {"code": -32601, "message": "Tool not found"}
            if mode == "/disconnect":
                type(self).offline = True
        payload = json.dumps(response, ensure_ascii=False).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Mcp-Session-Id", "bridge-test")
        self.end_headers()
        self.wfile.write(payload)


def main():
    server = ThreadingHTTPServer(("127.0.0.1", 0), Backend)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    requests = []

    def add(method, params=None):
        requests.append({"jsonrpc": "2.0", "id": len(requests) + 1, "method": method, "params": params or {}})

    def call(name, op):
        add("tools/call", {"name": name, "arguments": {"op": op}})

    add("initialize")
    add("tools/list")
    call("mc_input", "/help")
    call("mc_input", "/run")
    call("mc_profiler", "/guide")
    call("mc_input", "/failure")
    call("mc_profiler", "/missing")
    call("mc_input", "/disconnect")
    call("mc_input", "/help")
    call("mc_profiler", "/help")
    add("tools/list")
    add("ping")
    wire = "".join(json.dumps(request) + "\n" for request in requests)
    try:
        completed = subprocess.run(
            [sys.argv[1], "--host", "127.0.0.1", "--port", str(server.server_port)],
            input=wire,
            capture_output=True,
            encoding="utf-8",
            timeout=30,
            check=True,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
    finally:
        server.shutdown()
        server.server_close()
        thread.join()

    replies = [json.loads(line) for line in completed.stdout.splitlines()]
    assert [reply["id"] for reply in replies] == list(range(1, len(requests) + 1))
    results = [reply["result"] for reply in replies]
    names = {tool["name"] for tool in results[1]["tools"]}
    assert len(names) == 9 and {"mc_input", "mc_profiler", "capture_game_window"} <= names
    for index in (2, 3, 4, 7):
        assert results[index] == RESULT, "image, text and structured content must pass through unchanged"
    assert results[5] == FAILURE, "backend tool errors must remain unchanged"
    assert results[6]["structuredContent"]["error"]["code"] == "BACKEND_TOOL_UNAVAILABLE"
    for index in (8, 9):
        body = results[index]["structuredContent"]
        assert body["ok"] is False and body["error"]["code"] == "BACKEND_UNAVAILABLE"
        assert body["op"] == "/help", "offline help must not execute a local backend"
    assert results[10] == results[1], "the offline tool catalog must remain available"
    assert results[11] == {}, "the bridge must still handle requests after backend failure"
    assert Backend.calls == [request["params"] for request in requests[2:8]]

    check_declared_tools(sys.argv[1])
    print("PASS: tool catalog / forwarding / large image and logs / backend errors / offline recovery / plugin tools")


PLATFORM_KEY = {"win32": "windows", "darwin": "macos"}.get(sys.platform, "linux") + ".x86_64"

DECLARED_TOOL = {
    "name": "demo_count_entities",
    "description": "统计实体数量",
    "inputSchema": {"type": "object", "properties": {"dimension": {"type": "string"}}},
    "annotations": {"readOnlyHint": True, "title": "实体统计"},
}


def check_declared_tools(binary):
    """plugin.json 里声明的工具必须在 mcdk 没跑、后端也不存在时出现在 tools/list 里。

    MCP 客户端就是在那个时刻问这一次的，而且多数客户端不会再问第二次——所以这条
    路径不能依赖任何运行中的进程。
    """
    with tempfile.TemporaryDirectory() as workspace:
        root = Path(workspace)
        (root / "plugins" / "demo").mkdir(parents=True)
        (root / ".mcdev.json").write_text(
            json.dumps(
                {
                    "plugins": [
                        {"enable": True, "path": "plugins/demo", "id": "com.demo.tools"},
                        {"enable": False, "path": "plugins/off"},
                    ]
                }
            ),
            encoding="utf-8",
        )
        (root / "plugins" / "demo" / "plugin.json").write_text(
            json.dumps(
                {
                    "schema": 1,
                    "id": "com.demo.tools",
                    "version": "1.0.0",
                    "libraries": {PLATFORM_KEY: "demo.bin", PLATFORM_KEY + ".debug": "demo.bin"},
                    "mcpTools": [DECLARED_TOOL],
                },
                ensure_ascii=False,
            ),
            encoding="utf-8",
        )

        # 故意指向一个没人监听的端口：这条路径只读磁盘。
        completed = subprocess.run(
            [binary, "--host", "127.0.0.1", "--port", "1", "--project", str(root)],
            input=json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/list", "params": {}}) + "\n",
            capture_output=True,
            encoding="utf-8",
            timeout=30,
            check=True,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        tools = json.loads(completed.stdout.splitlines()[0])["result"]["tools"]
        by_name = {tool["name"]: tool for tool in tools}
        assert len(tools) == 10, f"9 个内置 + 1 个声明式，实际 {len(tools)}"
        assert DECLARED_TOOL["name"] in by_name, "plugin.json 声明的工具没进 tools/list"
        # 清单里写的就是列出来的，不经任何改写。
        assert by_name[DECLARED_TOOL["name"]] == DECLARED_TOOL
        assert not any(name.startswith("off_") for name in by_name), "enable=false 的插件不该贡献工具"


if __name__ == "__main__":
    main()
