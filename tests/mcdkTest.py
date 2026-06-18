import subprocess
result = subprocess.run(
    ["build\\x64-msvc-release\\tools\\mcdk\\mcdk.exe"],
    capture_output=True,
    text=True,
    encoding="utf-8",
    errors="replace",
)
print(result.stdout)
print(result.stderr)
print(f"Exit Code: {result.returncode}")