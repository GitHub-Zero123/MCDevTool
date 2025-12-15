# -*- coding: utf-8 -*-
from .Util import SystemSide
lambda: "By Zero123"

IsServerUser = False
ModDirName = SystemSide.__module__.split(".")[0]
QuModLibsPath = SystemSide.__module__[:SystemSide.__module__.rfind(".")]

class RuntimeService:
    _serverSystemList = []
    _clientSystemList = []
    _serverStarting = False
    _clientStarting = False
    # LOADER SYSTEM
    _serverLoadBefore = []      # type: list[function]
    _clientLoadBefore = []      # type: list[function]
    _serverLoadFinish = []      # type: list[function]
    _clientLoadFinish = []      # type: list[function]
    # THREAD ID
    _serverThreadID = None
    _clientThreadID = None
    _envPlayerId = None

def getUnderlineModDirName():
    # type: () -> str
    """ 获取下划线MOD目录名称 返回结果与preset内置变量__LQuModName__一致 (仅支持ascii字符串) """
    newStr = []     # type: list[int]
    for i, _charStr in enumerate(ModDirName):
        _char = ord(_charStr)
        if (_char >= 65 and _char <= 90):
            # 大写内容 进行处理
            if i > 0:
                newStr.append(ord("_"))
            newStr.append(_char + (97 - 65))
            continue
        # 常规小写内容 直接追加
        newStr.append(_char)
    return "".join((chr(x) for x in newStr))

def GET_THREAD_ID():
    """ 获取当前线程ID """
    from threading import current_thread
    return current_thread().ident

def IS_SERVER_THREAD():
    """ 检查是不是服务端线程 """
    return RuntimeService._serverThreadID != None and GET_THREAD_ID() == RuntimeService._serverThreadID

def IS_CLIENT_THREAD():
    """ 检查是不是客户端线程 """
    return RuntimeService._clientThreadID != None and GET_THREAD_ID() == RuntimeService._clientThreadID

def GET_THREAD_TYPE():
    """ 获取线程类型 -1.主线程 0.服务端线程 1.客户端线程 """
    tid = GET_THREAD_ID()
    if tid == RuntimeService._serverThreadID:
        return 0
    elif tid == RuntimeService._clientThreadID:
        return 1
    return -1