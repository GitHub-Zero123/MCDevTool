# -*- coding: utf-8 -*-
import mod.server.extraServerApi as serverApi
import mod.client.extraClientApi as clientApi
from .Config import GET_DEBUG_IPC_PORT
import socket
import threading
import json

def U16_BE(b):
    # type: (bytearray | str) -> int
    if isinstance(b, bytearray):
        return (b[0] << 8) | b[1]
    return (ord(b[0]) << 8) | ord(b[1])

def U32_BE(b):
    # type: (bytearray | str) -> int
    if isinstance(b, bytearray):
        return (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3]
    return (ord(b[0]) << 24) | (ord(b[1]) << 16) | (ord(b[2]) << 8) | ord(b[3])

class IPCSystem:
    def __init__(self, port=None):
        # type: (int | None) -> None
        self.port = port
        self.sock = None
        self.mLock = threading.Lock()
        self.handers = {}

    def registerHandler(self, typeID, handler):
        # type: (int, callable) -> None
        self.handers[typeID] = handler

    def updateHandlers(self, handlers):
        # type: (dict[int, callable]) -> None
        self.handers.update(handlers)

    def start(self):
        if self.sock or not self.port:
            return
        threading.Thread(target=self._threadListenLoop).start()

    def close(self):
        sock = None
        with self.mLock:
            sock = self.sock
            self.sock = None
        if sock:
            sock.shutdown(socket.SHUT_RDWR)
            sock.close()

    def _threadListenLoop(self):
        with self.mLock:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock = self.sock
        sock.connect(("localhost", self.port))
        sock.settimeout(0.05)
        print("[IPCSystem] 已连接到调试服务器，端口：" + str(self.port))
        # [2B TypeID][4B DataLength][Data]
        def _recvAll(sock, length):
            # type: (socket.socket, int) -> bytearray
            buf = bytearray()
            while len(buf) < length:
                more = sock.recv(length - len(buf))
                if not more:
                    raise EOFError("Socket closed before receiving all data")
                buf.extend(more)
            return buf
        while 1:
            try:
                header = _recvAll(sock, 6)
                typeID = U16_BE(header[0:2])
                dataLength = U32_BE(header[2:6])
                data = _recvAll(sock, dataLength)
            except socket.timeout:
                continue
            except EOFError:
                break
            except socket.error:
                break
            except Exception:
                import traceback
                traceback.print_exc()
                break
            if typeID in self.handers:
                try:
                    self.handers[typeID](data)
                except Exception:
                    import traceback
                    traceback.print_exc()
            else:
                print("[IPCSystem] 未知的TypeID数据包：" + str(typeID))
        with self.mLock:
            self.sock = None
        print("[IPCSystem] 连接已关闭")

_CL_GAME_COMP = None
_SR_GAME_COMP = None

def AUTO_RELOAD(_=None):
    from .Game import RELOAD_MOD
    if _CL_GAME_COMP:
        _CL_GAME_COMP.AddTimer(0, lambda: RELOAD_MOD())
        return

def FAST_RELOAD(data):
    from .Game import RELOAD_ONCE_MODULE
    pathList = json.loads(str(data))
    def _FAST_RELOAD():
        for path in pathList:
            if RELOAD_ONCE_MODULE(path):
                print("[FAST_RELOAD] Reloaded module successfully: \"" + path + "\"")
    if _CL_GAME_COMP:
        _CL_GAME_COMP.AddTimer(0, _FAST_RELOAD)
        return

def EXEC_CLIENT_CODE(data):
    code = compile(str(data), "<string>", "exec")
    def _EXEC_CODE():
        print("[CLIENT_CODE] Executed successfully: " + str(eval(code)))
    _CL_GAME_COMP.AddTimer(0, _EXEC_CODE)

def EXEC_SERVER_CODE(data):
    code = compile(str(data), "<string>", "exec")
    def _EXEC_CODE():
        print("[SERVER_CODE] Executed successfully: " + str(eval(code)))
    _SR_GAME_COMP.AddTimer(0, _EXEC_CODE)

def RELOAD_GAME(_=None):
    def _RELOAD_GAME():
        from .Game import RELOAD_WORLD
        print("[RELOAD_GAME] Reloading the game...")
        RELOAD_WORLD()
    _CL_GAME_COMP.AddTimer(0, _RELOAD_GAME)

_IPCSYSTEM = IPCSystem(GET_DEBUG_IPC_PORT())
_IPCSYSTEM.updateHandlers(
    {
        1: AUTO_RELOAD,
        2: FAST_RELOAD,
        3: EXEC_CLIENT_CODE,
        4: EXEC_SERVER_CODE,
        5: RELOAD_GAME,
    }
)

def ON_CLIENT_INIT():
    global _CL_GAME_COMP
    _CL_GAME_COMP = clientApi.GetEngineCompFactory().CreateGame(clientApi.GetLevelId())
    _IPCSYSTEM.start()

def ON_CLIENT_EXIT():
    _IPCSYSTEM.close()

def ON_SERVER_INIT():
    global _SR_GAME_COMP
    _SR_GAME_COMP = serverApi.GetEngineCompFactory().CreateGame(serverApi.GetLevelId())