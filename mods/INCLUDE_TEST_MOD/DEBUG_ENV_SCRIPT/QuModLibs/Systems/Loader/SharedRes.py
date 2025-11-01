# -*- coding: utf-8 -*-
from uuid import uuid4
from ...IN import ModDirName
from ...Util import QStruct

class CallObjData:
    def __init__(self, callObj, args = tuple(), kwargs = {}):
        self.callObj = callObj
        self.args = args
        self.kwargs = kwargs
        self._uid = None

class EmptyContext:
    pass

NAMESPACE = "Qu_" + ModDirName
SYSTEMNAME = "{}_QLoader_system".format(ModDirName)
SERVER_CALL_EVENT = "{}_QServer".format(ModDirName)
CLIENT_CALL_EVENT = "{}_QClient".format(ModDirName)

class EasyListener:
    def __init__(self):
        self._callQueue = []    # type: list[CallObjData]
        self._emptyContext = EmptyContext()
        self._QCustomAPI = {}   # type: dict[str, function]
    
    def regCustomApi(self, apiName="", func=lambda: None):
        """ 注册自定义API """
        self._QCustomAPI[apiName] = func
    
    def removeCustomApi(self, apiName=""):
        """ 删除指定API如果存在 """
        if apiName in self._QCustomAPI:
            del self._QCustomAPI[apiName]

    def getCustomApi(self, apiName=""):
        """ 获取自定义API如果存在 """
        return self._QCustomAPI.get(apiName)

    def localCall(self, apiName="", *args, **kwargs):
        """ 本地调用 请确保API函数存在注册 否则抛出异常 """
        return self._QCustomAPI[apiName](*args, **kwargs)

    def _systemCallListener(self, args={}):
        """ 系统call机制监听器(接收消息处理) """
        api = args["api"]
        ag = EasyListener._unPackRefArgs(args["args"])
        kwargs = EasyListener._unPackRefDictArgs(args["kw"])
        self._systemCallListenerHook(args)
        return self.localCall(api, *ag, **kwargs)
    
    def _systemCallListenerHook(self, _={}):
        pass

    @staticmethod
    def _unPackRefArgs(data):
        # type: (list) -> list
        """ Ref解包Args数据 """
        for i, v in enumerate(data):
            if QStruct.isSignData(v):
                data[i] = QStruct.loadSignData(v).onNetUnPack()
        return data

    @staticmethod
    def _unPackRefDictArgs(data):
        # type: (dict) -> dict
        """ Ref解包Dict Args数据 """
        for k, v in data.items():
            if QStruct.isSignData(v):
                data[k] = QStruct.loadSignData(v).onNetUnPack()
        return data

    @staticmethod
    def _packArgs(data):
        # type: (tuple | list) -> list
        """ 打包Args数据 """
        newDataList = []
        for v in data:
            if isinstance(v, QStruct):
                newDataList.append(v.signDumps())
                continue
            newDataList.append(v)
        return newDataList

    @staticmethod
    def _packDictArgs(data):
        # type: (dict) -> dict
        """ 打包Dict数据 keyName=xxx """
        newDict = {}
        for k, v in data.items():
            if isinstance(v, QStruct):
                newDict[k] = v.signDumps()
                continue
            newDict[k] = v
        return newDict

    def _packageCallArgs(self, apiName="", args=tuple(), kwargs=dict()):
        """ 打包API参数(发送消息处理) """
        return {"api":apiName,"args":EasyListener._packArgs(args),"kw":EasyListener._packDictArgs(kwargs)}

    def mallocRandomMetName(self):
        """ 动态分配随机方法名 """
        randomName = ""
        while not randomName or hasattr(self, randomName):
            randomName = "Q_{}".format(uuid4().hex)
        return randomName
    
    def _allocMethodWithOUTFunction(self, callFunc=lambda *_: None):
        """ 基于外部函数分配一个映射的内部方法(介于网易Listen必须依赖内部方法 故有了该方法) """
        newFuncName = self.mallocRandomMetName()
        newFunc = lambda *args, **kwargs: callFunc(*args, **kwargs)
        newFunc.__name__ = newFuncName
        setattr(self, newFuncName, newFunc)
        return newFunc
    
    def _delMethod(self, methodFunc=lambda *_: None):
        """ 对照与_allocMethodWithOUTFunction的反向删除 """
        try:
            delattr(self, methodFunc.__name__)
        except Exception:
            pass
    
    def nativeStaticListen(self, eventName="", callFunc=lambda *_: None):
        """ 原生静态监听注册 不支持运行时注销 """
        def _reg():
            self._easyListenForEvent(eventName, self, self._allocMethodWithOUTFunction(callFunc))
        self._callQueue.append(CallObjData(_reg))

    def nativeListen(self, eventName="", parent=None, callFunc=lambda *_: None, updateNow=False):
        # type: (str, object, object, bool) -> CallObjData | None
        """ 原生动态监听 当updateNow声明为False时将会添加到系统队列安全的等待注册 """
        if not parent:
            parent = self._emptyContext
        newFuncName = "QListen{}_{}".format(id(parent), callFunc.__name__)
        newFunc = lambda *args: callFunc(*args)
        newFunc.__name__ = newFuncName
        if hasattr(self, newFuncName):
            print("[Error] 请勿在单个可执行对象上监听重复的事件")
            return None
        def _reg():
            setattr(self, newFuncName, newFunc)
            self._easyListenForEvent(eventName, self, newFunc)
        waitCallObj = CallObjData(_reg)
        waitCallObj._uid = newFuncName
        self._callQueue.append(waitCallObj)
        if updateNow:
            self.unsafeUpdate(waitCallObj)
        return waitCallObj

    def unNativeListen(self, eventName="", parent=None, callFunc=lambda *_: None):
        # type: (str, object, object) -> None
        """ 取消特定方法的原生动态监听 """
        if not parent:
            parent = self._emptyContext
        newFuncName = "QListen{}_{}".format(id(parent), callFunc.__name__)
        if hasattr(self, newFuncName):
            # 已注册完毕的监听处理
            self._easyUnListenForEvent(eventName, self, getattr(self, newFuncName))
            delattr(self, newFuncName)
            return
        # 在队列中等待注册的监听处理
        self.removeCallObjByUid(newFuncName)

    def unsafeUpdate(self, callObjData):
        # type: (CallObjData) -> bool
        pass

    def _easyListenForEvent(self, eventName="", parent=None, func=lambda: None):
        pass

    def _easyUnListenForEvent(self, eventName="", parent=None, func=lambda: None):
        pass

    def removeCallObjByUid(self, _uid = ""):
        pass