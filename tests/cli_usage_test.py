"""mcdk 命令行写错时的行为：退出码 2，提示出错之处并指向对应的帮助。不需要游戏。"""
import subprocess
import sys
import tempfile

MCDK = sys.argv[1]

CASES = [
    (["runn"], "未知命令：runn，是不是 run？", "用法见 mcdk help\n"),
    (["run", "--bogus"], "无法识别的参数：--bogus", "用法见 mcdk help run"),
    (["wait", "--state", "foo"], "参数值无效", "用法见 mcdk help wait"),
    (["logs", "--since", "abc"], "参数值无法转换", "用法见 mcdk help logs"),
    (["lane"], "缺少子命令", "用法见 mcdk help lane"),
    (["call"], "缺少必需参数：tool", "用法见 mcdk help call"),
    (["help", "reload_script"], "是不是 reload_scripts？", "用法见 mcdk help\n"),
    (["call", "reload_script"], "未知工具：reload_script，是不是 reload_scripts？", "用法见 mcdk help\n"),
    (["call", "execute_code", "--args", '{"cod":"1"}'], "cod（是不是 code）", "用法见 mcdk help execute_code"),
    (["call", "execute_code", "--args", "{}"], "缺少必需参数：code", "用法见 mcdk help execute_code"),
    (["call", "read_logs", "--args", "[1]"], "工具参数必须是 JSON 对象", "用法见 mcdk help call"),
    (["lane", "remove"], "缺少必需参数：--session", "用法见 mcdk help lane remove"),
    (["help", "lane", "nosuch"], "未知命令或工具：lane nosuch", "用法见 mcdk help\n"),
    (["run", "--set", "wrold_name=a"], "wrold_name（是不是 world_name）", "用法见 mcdk help run"),
]


def main():
    failures = []
    # 空目录：没有会话、没有插件声明，校验发生在连接游戏之前。
    with tempfile.TemporaryDirectory() as project:
        for args, message, hint in CASES:
            result = subprocess.run(
                [MCDK, *args], cwd=project, capture_output=True, stdin=subprocess.DEVNULL, timeout=30
            )
            stderr = result.stderr.decode("utf-8", "replace").replace("\r\n", "\n")
            if result.returncode != 2 or message not in stderr or hint not in stderr:
                failures.append(f"{args}: exit={result.returncode} stderr={stderr!r}")
    if failures:
        print("FAILED:\n" + "\n".join(failures))
        return 1
    print("cli usage tests passed")
    return 0


sys.exit(main())
