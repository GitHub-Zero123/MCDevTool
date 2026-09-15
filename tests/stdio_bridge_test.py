#!/usr/bin/env python3
"""Verify the stdio proxy against a local fake MCP backend, without launching Minecraft."""

import json
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


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
    print("PASS: tool catalog / forwarding / large image and logs / backend errors / offline recovery")


if __name__ == "__main__":
    main()
