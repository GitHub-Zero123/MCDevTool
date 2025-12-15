# -*- coding: utf-8 -*-
import socket
import threading
import mod.client.extraClientApi as clientApi
from .Config import GET_DEBUG_IPC_PORT

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
            sock.close()

    def _threadListenLoop(self):
        with self.mLock:
            self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock = self.sock
        sock.connect(("localhost", self.port))
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
            except EOFError:
                break
            except Exception:
                import traceback
                traceback.print_exc()
                break
            typeID = U16_BE(header[0:2])
            dataLength = U32_BE(header[2:6])
            data = _recvAll(sock, dataLength)
            if typeID in self.handers:
                try:
                    self.handers[typeID](data)
                except Exception:
                    import traceback
                    traceback.print_exc()
            else:
                print("[IPCSystem] 未知的TypeID数据包：", typeID)
        with self.mLock:
            self.sock = None

GAME_COMP = None

def AUTO_RELOAD(_=None):
    from .Game import RELOAD_MOD
    if GAME_COMP:
        GAME_COMP.AddTimer(0, lambda: RELOAD_MOD())
        return

_IPCSYSTEM = IPCSystem(GET_DEBUG_IPC_PORT())
_IPCSYSTEM.updateHandlers(
    {
        1: AUTO_RELOAD
    }
)

def ON_CLIENT_INIT():
    global GAME_COMP
    GAME_COMP = clientApi.GetEngineCompFactory().CreateGame(clientApi.GetLevelId())
    _IPCSYSTEM.start()

def ON_CLIENT_EXIT():
    _IPCSYSTEM.close()
