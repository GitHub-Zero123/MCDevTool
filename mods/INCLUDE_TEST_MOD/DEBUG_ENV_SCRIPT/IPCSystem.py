# -*- coding: utf-8 -*-
import mod.server.extraServerApi as serverApi
import mod.client.extraClientApi as clientApi
from .Config import GET_DEBUG_IPC_PORT
import socket
import threading
import json
import traceback

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

IPC_JSON_REQUEST_TYPE = 100
IPC_JSON_RESPONSE_TYPE = 101


def _U16_BE_BYTES(v):
    # type: (int) -> str
    return chr((int(v) >> 8) & 0xFF) + chr(int(v) & 0xFF)


def _U32_BE_BYTES(v):
    # type: (int) -> str
    return chr((int(v) >> 24) & 0xFF) + chr((int(v) >> 16) & 0xFF) + chr((int(v) >> 8) & 0xFF) + chr(int(v) & 0xFF)


def _BYTES_TO_STR(data):
    if isinstance(data, bytearray):
        return str(data)
    return data


try:
    _UNICODE_TYPE = unicode
except NameError:
    _UNICODE_TYPE = str


def _TEXT_TO_BYTES(data):
    if isinstance(data, _UNICODE_TYPE) and not isinstance(data, str):
        return data.encode("utf-8")
    return data


class IPCSystem:
    def __init__(self, port=None):
        # type: (int | None) -> None
        self.port = port
        self.sock = None
        self.mLock = threading.Lock()
        self.mSendLock = threading.Lock()
        self.handers = {}
        self.jsonHandlers = {}

    def registerHandler(self, typeID, handler):
        # type: (int, callable) -> None
        self.handers[typeID] = handler

    def updateHandlers(self, handlers):
        # type: (dict[int, callable]) -> None
        self.handers.update(handlers)

    def registerJsonHandler(self, method, handler):
        # type: (str, callable) -> None
        self.jsonHandlers[method] = handler

    def updateJsonHandlers(self, handlers):
        # type: (dict[str, callable]) -> None
        self.jsonHandlers.update(handlers)

    def sendPacket(self, typeID, data=""):
        # type: (int, str | bytearray) -> bool
        if data is None:
            data = ""
        if isinstance(data, bytearray):
            data = str(data)
        data = _TEXT_TO_BYTES(data)
        length = len(data)
        packet = _U16_BE_BYTES(typeID) + _U32_BE_BYTES(length) + data
        with self.mLock:
            sock = self.sock
        if not sock:
            return False
        try:
            with self.mSendLock:
                sock.sendall(packet)
            return True
        except Exception:
            return False

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
                traceback.print_exc()
                break
            if typeID == IPC_JSON_REQUEST_TYPE:
                self._handleJsonRequest(data)
            elif typeID in self.handers:
                try:
                    self.handers[typeID](data)
                except Exception:
                    traceback.print_exc()
            else:
                print("[IPCSystem] 未知的TypeID数据包：" + str(typeID))
        with self.mLock:
            self.sock = None
        print("[IPCSystem] 连接已关闭")

    def _sendJsonResponse(self, requestId, ok=True, result=None, error=None):
        resp = {"id": requestId, "ok": ok}
        if ok:
            resp["result"] = result
        else:
            resp["error"] = error or {"code": "exception", "message": "Unknown JSON IPC error"}
        try:
            payload = json.dumps(resp, ensure_ascii=False)
        except Exception as e:
            payload = json.dumps({
                "id": requestId,
                "ok": False,
                "error": {
                    "code": "json_encode_error",
                    "message": str(e),
                    "traceback": traceback.format_exc()
                }
            }, ensure_ascii=False)
        try:
            return self.sendPacket(IPC_JSON_RESPONSE_TYPE, payload)
        except Exception:
            traceback.print_exc()
        return False

    def _handleJsonRequest(self, data):
        requestId = None
        try:
            req = json.loads(_BYTES_TO_STR(data))
            requestId = req.get("id", None)
            method = req.get("method", "")
            params = req.get("params", {})
            if method not in self.jsonHandlers:
                raise Exception("Unknown JSON IPC method: " + str(method))

            handler = self.jsonHandlers[method]
            state = {"done": False}
            stateLock = threading.Lock()

            def _callback(result=None, ok=True, error=None):
                with stateLock:
                    if state["done"]:
                        return False
                    state["done"] = True
                if ok:
                    return self._sendJsonResponse(requestId, True, result, None)
                return self._sendJsonResponse(requestId, False, None, error)

            if self._isJsonCallbackHandler(handler):
                handler(params, _callback)
            else:
                _callback(handler(params))
        except Exception as e:
            error = {
                "code": "exception",
                "message": str(e),
                "traceback": traceback.format_exc()
            }
            callback = locals().get("_callback", None)
            if callback:
                callback(None, False, error)
            else:
                self._sendJsonResponse(requestId, False, None, error)

    def _isJsonCallbackHandler(self, handler):
        code = getattr(handler, "func_code", None)
        if code is None:
            code = getattr(handler, "__code__", None)
        if code is None:
            return False
        argCount = getattr(code, "co_argcount", 0)
        if getattr(handler, "im_self", None) is not None or getattr(handler, "__self__", None) is not None:
            argCount -= 1
        defaults = getattr(handler, "func_defaults", None)
        if defaults is None:
            defaults = getattr(handler, "__defaults__", None)
        defaultCount = len(defaults) if defaults else 0
        requiredCount = argCount - defaultCount
        argNames = getattr(code, "co_varnames", ())
        if getattr(handler, "im_self", None) is not None or getattr(handler, "__self__", None) is not None:
            argNames = argNames[1:]
        argNames = argNames[:argCount]
        callbackNames = ("callback", "cb", "done", "reply", "respond")
        return requiredCount <= 2 and argCount >= 2 and len(argNames) >= 2 and argNames[1] in callbackNames

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

def _SAFE_REPR(value):
    try:
        return repr(value)
    except Exception:
        return "<repr failed>"


def _GET_TYPE_NAME(value):
    try:
        return type(value).__name__
    except Exception:
        return "unknown"


try:
    _LONG_TYPE = long
except NameError:
    _LONG_TYPE = int


def _JSON_SAFE_VALUE(value, depth=0):
    if depth > 8:
        return {"__type__": _GET_TYPE_NAME(value), "__repr__": _SAFE_REPR(value)}
    if value is None or isinstance(value, (bool, int, _LONG_TYPE, float)):
        return value
    if isinstance(value, _UNICODE_TYPE):
        return value
    if isinstance(value, str):
        return value
    if isinstance(value, (list, tuple)):
        return [_JSON_SAFE_VALUE(v, depth + 1) for v in value]
    if isinstance(value, dict):
        result = {}
        for k, v in value.items():
            try:
                key = k if isinstance(k, _UNICODE_TYPE) else str(k)
            except Exception:
                key = _SAFE_REPR(k)
            result[key] = _JSON_SAFE_VALUE(v, depth + 1)
        return result
    return {"__type__": _GET_TYPE_NAME(value), "__repr__": _SAFE_REPR(value)}


def _CODE_TEXT(codeText):
    if isinstance(codeText, _UNICODE_TYPE) and not isinstance(codeText, str):
        return codeText.encode("utf-8")
    return str(codeText)


def _EXEC_CODE_OBJECT(code, globalVars):
    exec("exec code in globalVars, globalVars")


def _EXEC_CODE_VALUE(codeText):
    codeText = _CODE_TEXT(codeText)
    globalVars = globals()
    try:
        code = compile(codeText, "<string>", "eval")
        return eval(code, globalVars, globalVars)
    except SyntaxError:
        code = compile(codeText, "<string>", "exec")
        resultName = "_result"
        hadResult = resultName in globalVars
        oldResult = globalVars.get(resultName, None)
        if hadResult:
            try:
                del globalVars[resultName]
            except Exception:
                pass
        try:
            _EXEC_CODE_OBJECT(code, globalVars)
            if resultName in globalVars:
                return globalVars[resultName]
            return None
        finally:
            if resultName in globalVars:
                try:
                    del globalVars[resultName]
                except Exception:
                    pass
            if hadResult:
                globalVars[resultName] = oldResult


def _EXEC_CODE_RESULT(codeText, sideName):
    value = _EXEC_CODE_VALUE(codeText)
    return {
        "side": sideName,
        "return_value": _JSON_SAFE_VALUE(value),
        "return_repr": _SAFE_REPR(value),
        "return_type": _GET_TYPE_NAME(value)
    }


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

def RELOAD_SHADERS(_=None):
    def _RELOAD_SHADERS():
        from .Game import RELOAD_SHADERS
        RELOAD_SHADERS()
    _CL_GAME_COMP.AddTimer(0, _RELOAD_SHADERS)

def RELOAD_ONCE_SHADERS(fileName):
    def _RELOAD_ONCE_SHADERS():
        if clientApi.ReloadOneShader(str(fileName)):
            print("[RELOAD_ONCE_SHADERS] Reloaded shaders successfully.")
            return
        print("[RELOAD_ONCE_SHADERS] Failed to reload shaders.")
    _CL_GAME_COMP.AddTimer(0, _RELOAD_ONCE_SHADERS)

def RELOAD_ADDON_AND_GAME(_=None):
    def _RELOAD_ADDON_AND_GAME():
        from .Game import RELOAD_WORLD, RELOAD_ADDON
        print("[RELOAD_ADDON_AND_GAME] Reloading the addon and the game...")
        RELOAD_ADDON()
        RELOAD_WORLD()
    _CL_GAME_COMP.AddTimer(0, _RELOAD_ADDON_AND_GAME)


def CALL_ON_CLIENT_THREAD(func, timeout=10.0):
    if not _CL_GAME_COMP:
        raise Exception("Client game component is not initialized")
    event = threading.Event()
    box = {"ok": False, "value": None, "error": None, "traceback": None}

    def _TASK():
        try:
            box["value"] = func()
            box["ok"] = True
        except Exception as e:
            box["error"] = str(e)
            box["traceback"] = traceback.format_exc()
        finally:
            event.set()

    _CL_GAME_COMP.AddTimer(0, _TASK)
    if not event.wait(timeout):
        raise Exception("Client thread task timed out")
    if not box["ok"]:
        raise Exception(str(box["error"]) + "\n" + str(box["traceback"]))
    return box["value"]


def CALL_ON_SERVER_THREAD(func, timeout=10.0):
    if not _SR_GAME_COMP:
        raise Exception("Server game component is not initialized")
    event = threading.Event()
    box = {"ok": False, "value": None, "error": None, "traceback": None}

    def _TASK():
        try:
            box["value"] = func()
            box["ok"] = True
        except Exception as e:
            box["error"] = str(e)
            box["traceback"] = traceback.format_exc()
        finally:
            event.set()

    _SR_GAME_COMP.AddTimer(0, _TASK)
    if not event.wait(timeout):
        raise Exception("Server thread task timed out")
    if not box["ok"]:
        raise Exception(str(box["error"]) + "\n" + str(box["traceback"]))
    return box["value"]


def JSON_EXECUTE_CODE(params, callback):
    isClient = params.get("is_client", True)
    codeText = params.get("code", "")
    timeout = params.get("timeout", 10.0)
    try:
        timeout = float(timeout)
    except Exception:
        timeout = 10.0

    def _RUN_CLIENT_CODE():
        return _EXEC_CODE_RESULT(codeText, "client")

    def _RUN_SERVER_CODE():
        return _EXEC_CODE_RESULT(codeText, "server")

    try:
        if isClient:
            result = CALL_ON_CLIENT_THREAD(_RUN_CLIENT_CODE, timeout)
        else:
            result = CALL_ON_SERVER_THREAD(_RUN_SERVER_CODE, timeout)
        callback(result)
    except Exception as e:
        callback(None, False, {
            "code": "execute_code_error",
            "message": str(e),
            "traceback": traceback.format_exc()
        })


def JSON_PING(params, callback):
    callback({
        "pong": True,
        "params": params,
        "client_ready": _CL_GAME_COMP is not None,
        "server_ready": _SR_GAME_COMP is not None
    })



_IPCSYSTEM = IPCSystem(GET_DEBUG_IPC_PORT())
_IPCSYSTEM.updateHandlers(
    {
        1: AUTO_RELOAD,
        2: FAST_RELOAD,
        3: EXEC_CLIENT_CODE,
        4: EXEC_SERVER_CODE,
        5: RELOAD_GAME,
        6: RELOAD_SHADERS,
        7: RELOAD_ONCE_SHADERS,
        8: RELOAD_ADDON_AND_GAME,
    }
)
_IPCSYSTEM.updateJsonHandlers(
    {
        "ping": JSON_PING,
        "execute_code": JSON_EXECUTE_CODE,
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