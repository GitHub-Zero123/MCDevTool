import platform
import subprocess
import shutil
import os
import mods.generate as generateMod

generateMod.generateCode()

def GET_MSVC_SETUP():
    # 检测 cl 是否已在环境变量中
    if shutil.which("cl"):
        return ""
    
    # 尝试通过 vswhere 查找 Visual Studio
    vswhere = os.path.join(
        os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
        "Microsoft Visual Studio", "Installer", "vswhere.exe"
    )
    
    if not os.path.exists(vswhere):
        return None

    try:
        # 获取最新安装的包含 VC 工具集的 VS 路径
        path = subprocess.check_output([
            vswhere, "-latest", "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
            "-property", "installationPath"
        ], encoding="utf-8", errors="ignore").strip()
        
        if path:
            # 构造 vcvars64.bat 路径
            bat = os.path.join(path, "VC", "Auxiliary", "Build", "vcvars64.bat")
            if os.path.exists(bat):
                return f"call \"{bat}\" && "
    except:
        pass
    return None

MSVC_SETUP_CMD = ""

def HAS_MSVC():
    global MSVC_SETUP_CMD
    setup = GET_MSVC_SETUP()
    if setup is not None:
        MSVC_SETUP_CMD = setup
        return True
    return False

def HAS_CLANG():
    return bool(
        shutil.which("clang-cl") or
        shutil.which("clang")
    )

if platform.system() == "Windows":
    if HAS_MSVC():
        preset = "x64-msvc-release"
        cmd_prefix = MSVC_SETUP_CMD
    elif HAS_CLANG():
        preset = "x64-clang-release"
        cmd_prefix = ""
else:
    preset = "x64-clang-release"
    cmd_prefix = ""

cmd = f"{cmd_prefix}cmake --preset {preset} && cmake --build --preset {preset}"
subprocess.check_call(cmd, shell=True)
