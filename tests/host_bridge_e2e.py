#!/usr/bin/env python3
"""Launch MCDK under a fake Host and verify clean client/server Python results."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import os
from pathlib import Path
import secrets
import socket
import struct
import subprocess
import sys
import time
from typing import Any


MAX_FRAME_BYTES = 16 * 1024 * 1024


class BridgeDisconnected(RuntimeError):
    pass


class BridgeConnection:
    def __init__(self, sock: socket.socket) -> None:
        self.sock = sock
        self.next_id = 1

    def close(self) -> None:
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.sock.close()

    def _recv_exact(self, size: int) -> bytes:
        chunks: list[bytes] = []
        remaining = size
        while remaining:
            chunk = self.sock.recv(remaining)
            if not chunk:
                raise BridgeDisconnected("MCDK closed the Host Bridge connection")
            chunks.append(chunk)
            remaining -= len(chunk)
        return b"".join(chunks)

    def receive(self) -> dict[str, Any]:
        length = struct.unpack(">I", self._recv_exact(4))[0]
        if length < 1 or length > MAX_FRAME_BYTES:
            raise BridgeDisconnected(f"invalid frame length: {length}")
        message = json.loads(self._recv_exact(length).decode("utf-8"))
        if not isinstance(message, dict):
            raise BridgeDisconnected("frame payload is not a JSON object")
        return message

    def send(self, message: dict[str, Any]) -> None:
        payload = json.dumps(message, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.sock.sendall(struct.pack(">I", len(payload)) + payload)

    def _handle_mcdk_request(self, message: dict[str, Any]) -> None:
        if "id" not in message:
            return
        if message.get("method") == "mcdk/ping":
            received_at = dt.datetime.now(dt.timezone.utc).isoformat().replace("+00:00", "Z")
            self.send({"jsonrpc": "2.0", "id": message["id"], "result": {"receivedAt": received_at}})
            return
        self.send(
            {
                "jsonrpc": "2.0",
                "id": message["id"],
                "error": {
                    "code": -32601,
                    "message": "Method not found",
                    "data": {"code": "METHOD_NOT_FOUND"},
                },
            }
        )

    def call(self, method: str, params: dict[str, Any]) -> dict[str, Any]:
        request_id = f"python-test:{self.next_id}"
        self.next_id += 1
        self.send({"jsonrpc": "2.0", "id": request_id, "method": method, "params": params})
        while True:
            message = self.receive()
            if message.get("id") == request_id and ("result" in message or "error" in message):
                return message
            if "method" in message:
                self._handle_mcdk_request(message)


def parse_args() -> argparse.Namespace:
    repository = Path(__file__).resolve().parents[1]
    default_mcdk = repository / "build" / "x64-msvc-release" / "tools" / "mcdk" / "mcdk.exe"
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mcdk", type=Path, default=default_mcdk, help="path to mcdk.exe")
    parser.add_argument("--cwd", type=Path, default=repository, help="MCDK working directory")
    parser.add_argument("--side", choices=("client", "server", "both"), default="both")
    parser.add_argument("--retry-seconds", type=float, default=2.0)
    parser.add_argument("--timeout-seconds", type=float, default=180.0)
    return parser.parse_args()


def create_listener() -> socket.socket:
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    if os.name == "nt" and hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
    listener.bind(("127.0.0.1", 0))
    listener.listen(4)
    listener.settimeout(1.0)
    return listener


def accept_bridge(
    listener: socket.socket,
    process: subprocess.Popen[Any],
    token: str,
    deadline: float,
) -> BridgeConnection:
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"MCDK exited before connecting (exit code {process.returncode})")
        try:
            sock, _ = listener.accept()
        except socket.timeout:
            continue
        sock.settimeout(15.0)
        connection = BridgeConnection(sock)
        try:
            initialize = connection.receive()
            if initialize.get("method") != "mcdk/initialize":
                raise RuntimeError("first bridge message is not mcdk/initialize")
            params = initialize.get("params", {})
            if params.get("authToken") != token:
                raise RuntimeError("MCDK initialize token does not match")
            connection.send(
                {
                    "jsonrpc": "2.0",
                    "id": initialize["id"],
                    "result": {
                        "protocolVersion": 1,
                        "heartbeatIntervalMs": 10000,
                        "heartbeatTimeoutMs": 30000,
                    },
                }
            )
            print(
                "connected: mcdkPid={} minecraftPid={} world={}".format(
                    params.get("mcdk", {}).get("pid"),
                    params.get("minecraft", {}).get("pid"),
                    params.get("world", {}).get("folderName"),
                )
            )
            return connection
        except Exception:
            connection.close()
            raise
    raise TimeoutError("timed out waiting for MCDK Host Bridge connection")


def execute_until_success(
    listener: socket.socket,
    process: subprocess.Popen[Any],
    token: str,
    sides: list[str],
    retry_seconds: float,
    deadline: float,
) -> None:
    connection: BridgeConnection | None = None
    completed: set[str] = set()
    attempt = 0
    while len(completed) != len(sides):
        if time.monotonic() >= deadline:
            raise TimeoutError(f"code execution did not become ready; completed={sorted(completed)}")
        if process.poll() is not None:
            raise RuntimeError(f"MCDK exited during test (exit code {process.returncode})")
        if connection is None:
            connection = accept_bridge(listener, process, token, deadline)

        side = next(item for item in sides if item not in completed)
        expected = {"bridge": "ok", "side": side, "value": 42}
        code = repr(expected)
        attempt += 1
        try:
            status_response = connection.call("game/ipc/is-ready", {})
            if "error" in status_response:
                raise RuntimeError(json.dumps(status_response["error"], ensure_ascii=False))
            ipc_status = status_response.get("result", {})
            if not ipc_status.get("debugCapabilityEnabled", False):
                raise RuntimeError("MCDK was launched without the debug capability")
            if not ipc_status.get("ready", False):
                print(
                    "attempt {}: game IPC not ready (clientCount={})".format(
                        attempt,
                        ipc_status.get("clientCount", 0),
                    )
                )
                time.sleep(retry_seconds)
                continue
            response = connection.call(
                "game/code/execute",
                {"code": code, "isClient": side == "client"},
            )
        except (BridgeDisconnected, ConnectionError, socket.timeout, OSError) as error:
            print(f"attempt {attempt}: bridge disconnected: {error}")
            connection.close()
            connection = None
            time.sleep(retry_seconds)
            continue

        if "result" in response:
            if response["result"] != expected:
                raise AssertionError(
                    "game/code/execute returned IPC wrapper data instead of the clean value: "
                    + json.dumps(response["result"], ensure_ascii=False)
                )
            completed.add(side)
            print(f"attempt {attempt}: {side} execution succeeded: {response['result']}")
            continue

        error = response.get("error", {})
        error_code = error.get("data", {}).get("code", "UNKNOWN_ERROR")
        print(f"attempt {attempt}: {side} not ready: {error_code}")
        if error_code == "DEBUG_CAPABILITY_DISABLED":
            raise RuntimeError("MCDK was launched without the debug capability")
        if error_code not in ("GAME_WORLD_NOT_READY", "HANDLER_TIMEOUT"):
            raise RuntimeError(json.dumps(error, ensure_ascii=False))
        time.sleep(retry_seconds)

    if connection is not None:
        connection.close()


def stop_process_tree(process: subprocess.Popen[Any]) -> None:
    if process.poll() is not None:
        return
    if os.name == "nt":
        subprocess.run(
            ["taskkill", "/PID", str(process.pid), "/T", "/F"],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=False,
        )
    else:
        process.terminate()
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


def main() -> int:
    args = parse_args()
    mcdk_path = args.mcdk.resolve()
    if not mcdk_path.is_file():
        raise FileNotFoundError(f"MCDK executable not found: {mcdk_path}")

    listener = create_listener()
    token = secrets.token_hex(32)
    environment = os.environ.copy()
    environment["MCDEV_HOST_PORT"] = str(listener.getsockname()[1])
    environment["MCDEV_HOST_TOKEN"] = token
    creation_flags = subprocess.CREATE_NEW_PROCESS_GROUP if os.name == "nt" else 0
    process = subprocess.Popen(
        [str(mcdk_path)],
        cwd=str(args.cwd.resolve()),
        env=environment,
        creationflags=creation_flags,
    )
    sides = ["client", "server"] if args.side == "both" else [args.side]
    deadline = time.monotonic() + args.timeout_seconds
    try:
        execute_until_success(
            listener,
            process,
            token,
            sides,
            args.retry_seconds,
            deadline,
        )
        print("Host Bridge end-to-end test passed; stopping MCDK process tree")
        return 0
    finally:
        listener.close()
        stop_process_tree(process)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
    except Exception as error:
        print(f"host_bridge_e2e failed: {error}", file=sys.stderr)
        raise SystemExit(1)
