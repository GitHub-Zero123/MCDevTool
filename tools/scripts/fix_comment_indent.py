#!/usr/bin/env python3
"""把「缩进比周围代码浅的纯注释行」对齐到所属代码块。

场景：注释是在较低缩进的上下文里写下的，之后被移动/粘贴进更深的代码块，
但注释行没跟着缩进，于是块里出现一批顶格（或半顶格）的 // 注释。

判定规则：
    一段连续的纯注释行（// 开头）整体作为一个单元，取它的**最浅**缩进与
    上下两个「有效层级」比较；比两者都浅才判为未对齐，目标层级取两者中较浅的那个。

    有效层级：上方代码行若以 { 结尾（注释位于该块内部），层级 = 其缩进 + 一级；
    下方代码行若以 } 开头（注释是该块最后的内容），层级 = 其缩进 + 一级；
    其余情况即该行自身缩进。这个不对称是刻意的：
    「class Foo {」上方的注释属于 class 本身，不该再往右缩进一级。

    只修正比周围更浅的注释，从不反向缩浅——注释是块体本身（例如空 catch 里的
    一行说明）时保持原样。

用法：
    python tools/scripts/fix_comment_indent.py                 # 扫描并列出问题（不改动）
    python tools/scripts/fix_comment_indent.py --apply         # 就地修正
    python tools/scripts/fix_comment_indent.py --path src      # 只处理指定路径
"""

from __future__ import annotations

import argparse
import os
import sys
from dataclasses import dataclass
from typing import Iterator

DEFAULT_ROOTS = ("src", "include", "tools", "sdk", "tests", "components", "mods")

EXTENSIONS = (".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp")

# 目录名黑名单（任一层命中即整体跳过）
SKIP_DIRS = {
    ".git",
    ".vs",
    ".vscode",
    "build",
    "bins",
    "third_party",
    "libs",
    "temps",
    "node_modules",
    "__pycache__",
    "Resource",
}


@dataclass
class Line:
    number: int  # 1-based
    raw: str  # 含行尾
    body: str  # 不含行尾
    eol: str  # "\r\n" / "\n" / ""
    kind: str  # "blank" | "comment" | "code"
    indent: int = 0  # 前导空白宽度（tab 记 4）

    def __post_init__(self) -> None:
        self.indent = indent_width(self.body)


@dataclass
class Finding:
    path: str
    first: int  # 注释单元首行号
    last: int  # 注释单元末行号
    current: int  # 当前缩进（单元内最浅的那个）
    mixed: bool  # 单元内缩进是否不一致
    prev_indent: int | None  # 上方有效层级
    next_indent: int | None  # 下方有效层级
    target: int  # 目标缩进
    reason: str


def indent_width(line: str) -> int:
    width = 0
    for ch in line:
        if ch == " ":
            width += 1
        elif ch == "\t":
            width += 4
        else:
            break
    return width


def leading_ws(line: str) -> str:
    out = []
    for ch in line:
        if ch in " \t":
            out.append(ch)
        else:
            break
    return "".join(out)


def is_comment_body(body: str) -> bool:
    return body.lstrip(" \t").startswith("//")


def iter_files(roots: list[str]) -> Iterator[str]:
    for root in roots:
        if os.path.isfile(root):
            yield root
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
            for name in filenames:
                if name.endswith(EXTENSIONS):
                    yield os.path.join(dirpath, name)


def classify(text: str) -> list[Line]:
    """按行切分，并标记每行是空白 / 纯注释 / 代码。

    会跳过块注释 /* */ 与字符串（含原始字符串）内部的区域，
    避免把字符串里的 "//" 误判成注释。
    """
    lines: list[Line] = []
    in_block_comment = False
    raw_delim: str | None = None

    for index, raw in enumerate(text.splitlines(keepends=True), start=1):
        body = raw.rstrip("\r\n")
        eol = raw[len(body):]

        if raw_delim is not None:
            kind = "blank" if not body.strip() else "code"
            if f"){raw_delim}\"" in body:
                raw_delim = None
            lines.append(Line(index, raw, body, eol, kind))
            continue

        if in_block_comment:
            kind = "blank" if not body.strip() else "code"
            if "*/" in body:
                in_block_comment = False
            lines.append(Line(index, raw, body, eol, kind))
            continue

        stripped = body.strip()
        if not stripped:
            kind = "blank"
        elif stripped.startswith("//"):
            kind = "comment"
        else:
            kind = "code"

        # 只有在非注释行里才需要继续扫描状态（注释行内部的状态另行处理）
        scan = body
        if kind == "comment":
            lines.append(Line(index, raw, body, eol, kind))
            continue

        pos = 0
        while pos < len(scan):
            ch = scan[pos]
            if ch == "/" and pos + 1 < len(scan):
                nxt = scan[pos + 1]
                if nxt == "/":
                    break  # 行注释，行尾结束
                if nxt == "*":
                    end = scan.find("*/", pos + 2)
                    if end == -1:
                        in_block_comment = True
                        break
                    pos = end + 2
                    continue
            if ch == "R" and pos + 1 < len(scan) and scan[pos + 1] == '"':
                close = scan.find("(", pos + 2)
                if close != -1:
                    delim = scan[pos + 2:close]
                    marker = f"){delim}\""
                    end = scan.find(marker, close + 1)
                    if end == -1:
                        raw_delim = delim
                        break
                    pos = end + len(marker)
                    continue
            if ch in "\"'":
                quote = ch
                pos += 1
                while pos < len(scan):
                    if scan[pos] == "\\":
                        pos += 2
                        continue
                    if scan[pos] == quote:
                        pos += 1
                        break
                    pos += 1
                continue
            pos += 1

        lines.append(Line(index, raw, body, eol, kind))

    return lines


def find_units(lines: list[Line]) -> list[tuple[int, int]]:
    """返回纯注释行的极大连续区间（下标，闭区间）。"""
    units: list[tuple[int, int]] = []
    start: int | None = None
    for i, line in enumerate(lines):
        if line.kind == "comment":
            if start is None:
                start = i
        else:
            if start is not None:
                units.append((start, i - 1))
                start = None
    if start is not None:
        units.append((start, len(lines) - 1))
    return units


def nearest_code(lines: list[Line], start: int, step: int) -> Line | None:
    i = start
    while 0 <= i < len(lines):
        if lines[i].kind == "code":
            return lines[i]
        i += step
    return None


def indent_step() -> int:
    return 4


def prev_level(line: Line | None) -> int | None:
    """注释所处块的层级，由上方最近代码行推断。"""
    if line is None:
        return None
    if line.body.rstrip().endswith("{"):
        return line.indent + indent_step()
    return line.indent


def next_level(line: Line | None) -> int | None:
    """注释所处块的层级，由下方最近代码行推断。

    下方若是块的收尾（} / }; / }; 之类），注释是该块最后的内容，层级再深一级。
    下方若是块的开启（class Foo {），注释属于这个构造本身，不加级。
    """
    if line is None:
        return None
    stripped = line.body.strip()
    if stripped.startswith("}"):
        return line.indent + indent_step()
    return line.indent


def analyse(path: str) -> list[Finding]:
    with open(path, "r", encoding="utf-8", newline="") as handle:
        text = handle.read()
    lines = classify(text)
    findings: list[Finding] = []

    for first, last in find_units(lines):
        indents = [lines[i].indent for i in range(first, last + 1)]
        current = min(indents)
        mixed = len(set(indents)) > 1
        prev_indent = prev_level(nearest_code(lines, first - 1, -1))
        next_indent = next_level(nearest_code(lines, last + 1, +1))

        levels = [lv for lv in (prev_indent, next_indent) if lv is not None]
        if not levels:
            continue

        # 只修「比周围都浅」的注释：更深说明它是某个块的块体内容，原样保留
        target = min(levels)
        if current > target:
            continue
        if current == target and not mixed:
            continue

        if mixed and current == target:
            reason = "块内缩进不一致，统一到所在层级"
        elif mixed:
            reason = "块内缩进不一致，取最浅层级对齐"
        elif prev_indent is not None and next_indent is not None and prev_indent != next_indent:
            reason = f"取上下较浅层级（上 {prev_indent} / 下 {next_indent}）"
        else:
            reason = "对齐所在代码块"

        findings.append(
            Finding(
                path=path,
                first=lines[first].number,
                last=lines[last].number,
                current=current,
                mixed=mixed,
                prev_indent=prev_indent,
                next_indent=next_indent,
                target=target,
                reason=reason,
            )
        )

    return findings


def apply_fix(finding: Finding) -> None:
    with open(finding.path, "r", encoding="utf-8", newline="") as handle:
        text = handle.read()
    lines = classify(text)
    pad = " " * (finding.target or 0)

    changed = False
    for i in range(finding.first - 1, finding.last):
        line = lines[i]
        if leading_ws(line.body) != pad:
            line.body = pad + line.body.lstrip(" \t")
            line.raw = line.body + line.eol
            changed = True

    if not changed:
        return

    with open(finding.path, "w", encoding="utf-8", newline="") as handle:
        handle.write("".join(line.raw for line in lines))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--apply", action="store_true", help="就地修正（默认只报告）")
    parser.add_argument("--path", action="append", default=None, help="要扫描的路径，可重复；默认为项目主要源码目录")
    parser.add_argument("--quiet", action="store_true", help="只输出统计")
    args = parser.parse_args()

    roots = args.path or [r for r in DEFAULT_ROOTS if os.path.exists(r)]
    findings: list[Finding] = []
    for path in sorted(set(iter_files(roots))):
        findings.extend(analyse(path))

    findings.sort(key=lambda f: (f.path, f.first))

    for finding in findings:
        if args.quiet:
            continue
        rel = os.path.relpath(finding.path)
        span = f"{finding.first}" if finding.first == finding.last else f"{finding.first}-{finding.last}"
        print(
            f"{rel}:{span}  缩进 {finding.current} -> {finding.target}"
            f"  (上 {finding.prev_indent} / 下 {finding.next_indent})  {finding.reason}"
        )

    if args.apply:
        for finding in findings:
            apply_fix(finding)
        print(f"\n已修正 {len(findings)} 处注释缩进，涉及 {len({f.path for f in findings})} 个文件。")
    else:
        print(f"\n发现 {len(findings)} 处待对齐注释，涉及 {len({f.path for f in findings})} 个文件（未改动）。")

    return 0


if __name__ == "__main__":
    sys.exit(main())
