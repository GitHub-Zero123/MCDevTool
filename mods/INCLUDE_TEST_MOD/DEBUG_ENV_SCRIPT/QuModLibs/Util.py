# -*- coding: utf-8 -*-
from functools import wraps
from time import time
import pickle as _pickle

class SystemSide(object):
    def __init__(self, Path, SystemName = None):
        self.SystemName = SystemName # 绑定系统
        self.Path = Path

ModDirName = SystemSide.__module__.split(".")[0]

def errorPrint(charPtr):
    """ 异常输出 """
    print("[Error] "+str(charPtr))

def TRY_EXEC_FUN(funObj, *args, **kwargs):
    try:
        return funObj(*args, **kwargs)
    except Exception as e:
        import traceback
        print("TRY_EXEC发生异常: {}".format(e))
        traceback.print_exc()

def getObjectPathName(_callObj = lambda: None):
    # type: (object) -> str
    """ 获取可执行对象的目录名 """
    return ".".join((_callObj.__module__, _callObj.__name__))

class QStruct:
    """ 结构体 用于通用数据模型约定(即不涉及任何API) 应定义在Server/Client以外的通用文件 同理Struct也不应该持有任何涉及端侧API的内容 """
    _SIGN_FORMAT = "_QSTRUCT[{}]"
    def dumps(self):
        """ 序列化对象 """
        return _pickle.dumps(self)

    def signDumps(self):
        """ 带有特征签名的序列化 """
        data = self.dumps()
        return [QStruct._SIGN_FORMAT.format(hex(hash(data))), data]

    @staticmethod
    def isSignData(data):
        """ 校验数据 """
        if not isinstance(data, list) or len(data) != 2:
            return False
        signKey = data[0]
        if isinstance(signKey, str):
            dataObj = data[1]
            return signKey == QStruct._SIGN_FORMAT.format(hex(hash(dataObj)))
        return False

    @staticmethod
    def loads(data):
        # type: (str) -> QStruct
        """ 反序列化加载对象 """
        return _pickle.loads(data)

    @staticmethod
    def loadSignData(data):
        # type: (list) -> QStruct
        """ 反序列化加载Sign对象表(不会校验) """
        return _pickle.loads(data[1])

    def onNetUnPack(self):
        return self

class QRefStruct(QStruct):
    """ 万能引用 """
    def __init__(self, refObject):
        self.ref = refObject

    def onNetUnPack(self):
        return self.ref

class QListStruct(QStruct, list):
    """ List容器结构 """
    def onNetUnPack(self):
        return list(self)

class QDictStruct(QStruct, dict):
    """ Dict容器结构 """
    def onNetUnPack(self):
        return dict(self)

class QTupleStruct(QStruct, tuple):
    """ Tuple容器结构 """
    def onNetUnPack(self):
        return tuple(self)