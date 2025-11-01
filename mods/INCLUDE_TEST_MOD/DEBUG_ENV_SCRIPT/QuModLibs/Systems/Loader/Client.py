# -*- coding: utf-8 -*-
import mod.client.extraClientApi as clientApi
from ...Util import errorPrint, TRY_EXEC_FUN, getObjectPathName
from ...IN import RuntimeService
from .SharedRes import (
    CallObjData,
    EasyListener,
    SERVER_CALL_EVENT,
    CLIENT_CALL_EVENT,
    NAMESPACE,
    SYSTEMNAME
)
lambda: "By Zero123"
ClientSystem = clientApi.GetClientSystemCls()
engineSpaceName, engineSystemName = clientApi.GetEngineNamespace(), clientApi.GetEngineSystemName()

def clientImportModule(filePath):
    """ 客户端文件导入 """
    return clientApi.ImportModule(filePath)

class LoaderSystem(ClientSystem, EasyListener):
    """ QuMod加载器系统
        加载器承担了系统文件的加载以及事件监听 系统通信
    """
    @staticmethod
    def getSystem():
        # type: () -> LoaderSystem
        """ 获取加载器系统 如果未注册将会自动注册并返回 """
        system = clientApi.GetSystem(NAMESPACE, SYSTEMNAME)
        if system:
            return system
        return clientApi.RegisterSystem(NAMESPACE, SYSTEMNAME, LoaderSystem.__module__ + "." + LoaderSystem.__name__)

    _REG_CALL_FUNCS = {}
    _REG_STATIC_LISTEN_FUNCS = {}
    _DY_IMP_CACHE = {}

    @staticmethod
    def dyImportModule(modulePath):
        if not modulePath in LoaderSystem._DY_IMP_CACHE:
            LoaderSystem._DY_IMP_CACHE[modulePath] = clientImportModule(modulePath)
        return LoaderSystem._DY_IMP_CACHE[modulePath]

    @staticmethod
    def REG_DESTROY_CALL_FUNC(func=lambda: None):
        """ 适用于静态函数的注册销毁时回调 """
        keyName = getObjectPathName(func)
        if not keyName in LoaderSystem._REG_CALL_FUNCS:
            # callFunc = lambda: LoaderSystem._REG_CALL_FUNCS[keyName]()
            LoaderSystem.getSystem().addDestroyCall(func)
        LoaderSystem._REG_CALL_FUNCS[keyName] = func
    
    @staticmethod
    def REG_STATIC_LISTEN_FUNC(eventName="", funcObj=lambda: None):
        """ 注册静态监听函数 """
        keyName = getObjectPathName(funcObj)
        if not keyName in LoaderSystem._REG_STATIC_LISTEN_FUNCS:
            # callFunc = lambda *args: LoaderSystem._REG_STATIC_LISTEN_FUNCS[keyName](*args)
            LoaderSystem.getSystem().nativeStaticListen(eventName, funcObj)
        LoaderSystem._REG_STATIC_LISTEN_FUNCS[keyName] = funcObj
    
    def __init__(self, namespace, systemName):
        ClientSystem.__init__(self, namespace, systemName)
        EasyListener.__init__(self)
        RuntimeService._clientStarting = True
        self.namespace = namespace
        self.systemName = systemName
        self._systemList = RuntimeService._clientSystemList
        self._initState = False
        self._regInitState = False
        self._waitTime = 0.0
        self._callQueue = []    # type: list[CallObjData]
        self._onDestroyCall = []
        self._onDestroyCall_LAST = []
        """ 后置销毁触发 通常是内部使用确保在用户业务之后执行 """
        self._initSystemListen()
        self.systemInit()
    
    def _initSystemListen(self):
        self.ListenForEvent(NAMESPACE, SYSTEMNAME, SERVER_CALL_EVENT, self, self._systemCallListener)

    def _easyListenForEvent(self, eventName="", parent=None, func=lambda: None):
        return self.ListenForEvent(engineSpaceName, engineSystemName, eventName, parent, func)

    def _easyUnListenForEvent(self, eventName="", parent=None, func=lambda: None):
        return self.UnListenForEvent(engineSpaceName, engineSystemName, eventName, parent, func)

    def sendCall(self, apiName="", args=tuple(), kwargs=dict()):
        """ 向服务器端请求调用 """
        sendData = self._packageCallArgs(apiName, args, kwargs)
        self.NotifyToServer(CLIENT_CALL_EVENT, sendData)

    def addDestroyCall(self, funObj, doubleCheck=True):
        """ 添加销毁触发 """
        if doubleCheck and funObj in self._onDestroyCall:
            return
        self._onDestroyCall.append(funObj)

    def removeDestroyCall(self, funObj):
        """ 移除销毁触发 """
        if funObj in self._onDestroyCall:
            self._onDestroyCall.remove(funObj)
    
    def Destroy(self):
        # 用户级destroy执行
        for obj in self._onDestroyCall:
            TRY_EXEC_FUN(obj)
        self._onDestroyCall = []
        # 高权限destroy执行
        for obj in self._onDestroyCall_LAST:
            TRY_EXEC_FUN(obj)
        self._onDestroyCall_LAST = []
        RuntimeService._clientStarting = False

    def getSystemList(self):
        # type: () -> list[tuple[str, str | None]]
        return self._systemList

    def removeCallObjByUid(self, _uid = ""):
        """ 尝试移除特定uid的callObj 如果存在 """
        for i, obj in enumerate(self._callQueue):
            if obj._uid == _uid:
                del self._callQueue[i]
                break

    def proxyRegister(self, funcObj):
        """ 代理注册 """
        from functools import wraps
        @wraps(funcObj)
        def newFun(*args, **kwargs):
            callObj = CallObjData(funcObj, args, kwargs)
            self._callQueue.append(callObj)
            return callObj
        return newFun
    
    def unsafeUpdate(self, callObjData):
        # type: (CallObjData) -> bool
        """ 不安全的强制刷新 """
        if callObjData in self._callQueue:
            self._callQueue.remove(callObjData)
            callObjData.callObj(*callObjData.args, **callObjData.kwargs)
            return True
        return False

    def Update(self):
        self.regSystemInit()
        if self._callQueue:
            for obj in self._callQueue:
                try:
                    obj.callObj(*obj.args, **obj.kwargs)
                except Exception as e:
                    errorPrint("{} call执行异常 {}".format(obj.callObj, e))
                    import traceback
                    traceback.print_exc()
            self._callQueue = []
        return ClientSystem.Update(self)

    def systemInit(self):
        self._initState = True

    def regSystemInit(self):
        """ 系统信息注册初始化 """
        if self._regInitState:
            return
        # 加载Before事件
        for funcObj in RuntimeService._clientLoadBefore:
            TRY_EXEC_FUN(funcObj)
        # 因历史原因systemName已废弃 此处仅兼容旧版项目
        for path, _ in self._systemList:
            sysObj = None
            try:
                sysObj = clientImportModule(path)
                if sysObj == None:
                    errorPrint("[客户端] 系统文件加载失败(API异常): {}".format(path))
                    continue
            except Exception as e:
                errorPrint("[客户端] 系统文件错误: {} ({})".format(path, e))
                import traceback
                traceback.print_exc()
                continue
            # if not systemName: systemName = uuid4().hex
        self._regInitState = True
        # 加载Finish事件
        for funcObj in RuntimeService._clientLoadFinish:
            TRY_EXEC_FUN(funcObj)