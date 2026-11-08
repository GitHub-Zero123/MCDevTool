"""冒充一个运行中的会话（会话记录指向本进程，端口上跑最小 MCP），检查 call/help 在工程配置里
找不到工具时回退到会话实际发布的工具清单，并且每次连接结束都关闭 MCP 会话。"""
import ctypes, json, os, pathlib, subprocess, sys, tempfile, threading
from ctypes import wintypes as w
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MCDK = sys.argv[1]
k32 = ctypes.WinDLL("kernel32")
k32.GetCurrentProcess.restype = w.HANDLE
k32.GetProcessTimes.argtypes = [w.HANDLE] + [ctypes.POINTER(w.FILETIME)] * 4
ft = [w.FILETIME() for _ in range(4)]
k32.GetProcessTimes(k32.GetCurrentProcess(), *[ctypes.byref(f) for f in ft])
start_time = (ft[0].dwHighDateTime << 32) | ft[0].dwLowDateTime

TOOL = {"name": "alt_only_tool", "description": "只在会话里存在的工具",
        "inputSchema": {"type": "object", "properties": {"n": {"type": "number"}}, "required": ["n"]}}
calls, deletes = [], []


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def reply(self, body, session=None):
        data = json.dumps(body).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        if session:
            self.send_header("Mcp-Session-Id", session)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_DELETE(self):
        deletes.append(self.headers.get("Mcp-Session-Id"))
        self.send_response(200)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        method = req.get("method")
        if "id" not in req:
            self.send_response(202)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if method == "initialize":
            return self.reply({"jsonrpc": "2.0", "id": req["id"], "result": {"protocolVersion": "2025-03-26"}}, "s1")
        if method == "tools/list":
            return self.reply({"jsonrpc": "2.0", "id": req["id"], "result": {"tools": [TOOL]}})
        if method == "tools/call":
            calls.append(req["params"])
            return self.reply({"jsonrpc": "2.0", "id": req["id"],
                               "result": {"content": [{"type": "text", "text": "ok"}], "isError": False}})
        self.reply({"jsonrpc": "2.0", "id": req["id"], "error": {"code": -32601, "message": "no"}})


server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
results = []


def run(*args):
    r = subprocess.run([MCDK, *args], cwd=project, capture_output=True, timeout=30, stdin=subprocess.DEVNULL)
    return r.returncode, r.stdout.decode("utf-8", "replace") + r.stderr.decode("utf-8", "replace")


def check(name, ok, detail=""):
    results.append(ok)
    print(("PASS " if ok else "FAIL ") + name + ("  | " + detail.strip()[:200] if detail else ""))


with tempfile.TemporaryDirectory() as project:
    sessions = pathlib.Path(project, ".mcdev", "sessions")
    sessions.mkdir(parents=True)
    pathlib.Path(project, ".mcdev.json").write_text("{}")
    (sessions / "alt.json").write_text(json.dumps({
        "name": "alt", "state": "game_ready", "mcdkPid": os.getpid(), "mcdkStartTime": start_time,
        "gamePid": 0, "mcpHost": "127.0.0.1", "mcpPort": server.server_address[1]}))

    code, out = run("call", "alt_only_tool", "--session", "alt", "--args", '{"n":3}')
    check("call 回退到会话的工具清单", code == 0 and calls == [{"name": "alt_only_tool", "arguments": {"n": 3}}], f"exit={code} {out}")
    code, out = run("call", "alt_only_tool", "--args", '{"m":3}')
    check("会话里的工具也校验参数名", code == 2 and "m（是不是 n）" in out and "可用参数：n" in out, f"exit={code} {out}")
    code, out = run("call", "alt_only_tool", "--args", "{}")
    check("会话里的工具也校验必需参数", code == 2 and "缺少必需参数：n" in out, f"exit={code} {out}")
    code, out = run("call", "nope_tool", "--args", "{}")
    check("两边都没有的工具仍是用法错误", code == 2 and "未知工具" in out, f"exit={code} {out}")
    code, out = run("help", "alt_only_tool")
    check("help 能查到会话里的工具", code == 0 and "只在会话里存在的工具" in out, f"exit={code} {out}")
    check("每次连接都关闭了 MCP 会话", len(deletes) >= 2 and all(d == "s1" for d in deletes), str(deletes))

server.shutdown()
print(f"{sum(results)}/{len(results)} passed")
sys.exit(0 if all(results) else 1)
