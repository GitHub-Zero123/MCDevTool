# -*- coding: utf-8 -*-
import threading
import sys

class STD_OUT_WRAPPER(object):
    def __init__(self, baseIO):
        self.baseIO = baseIO
        self.writeLock = threading.Lock()
        self._buffer = []

    def __getattr__(self, name):
        return getattr(self.baseIO, name)

    def write(self, data):
        with self.writeLock:
            parts = data.splitlines(True)
            for part in parts:
                if part.endswith("\n"):
                    if self._buffer:
                        line = "".join(self._buffer) + part
                        self._buffer = []
                    else:
                        line = part
                    self.baseIO.write("[Python] " + line)
                else:
                    self._buffer.append(part)

    def close(self):
        return self.baseIO.close()

    def writelines(self, lines):
        for line in lines:
            self.write(line)

    def fileno(self):
        return self.baseIO.fileno()

stdout = sys.stdout
stderr = sys.stderr


print("aaaa" * 1000)