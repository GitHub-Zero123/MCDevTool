# # -*- coding: utf-8 -*-

def func():
    print("Hello, World!")

code = """
print("你好，世界！")
func()
func2 = lambda: "Hello from lambda!"
print(func2())
"""

data = compile(code, "<string>", "exec")
eval(data)