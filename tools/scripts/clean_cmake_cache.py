#!/usr/bin/env python3
"""Remove validated CMake build trees while preserving shared downloads."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import stat
import sys


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
BUILD_ROOT = (REPOSITORY_ROOT / "build").resolve()
DOWNLOAD_CACHE = REPOSITORY_ROOT / ".cache" / "mcdev-deps" / "downloads"
CMAKE_HOME_PREFIX = "CMAKE_HOME_DIRECTORY:INTERNAL="
DEFAULT_BUILD_NAME = "x64-msvc-release"


def same_path(left: Path, right: Path) -> bool:
    return os.path.normcase(str(left.resolve())) == os.path.normcase(str(right.resolve()))


def cmake_source_directory(build_directory: Path) -> Path | None:
    cache = build_directory / "CMakeCache.txt"
    if not cache.is_file():
        return None

    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith(CMAKE_HOME_PREFIX):
            value = line[len(CMAKE_HOME_PREFIX) :].strip()
            return Path(value) if value else None
    return None


def validate_build_name(name: str) -> Path:
    if not name or Path(name).name != name or name in {".", ".."}:
        raise ValueError(f"invalid build name: {name!r}")

    target = (BUILD_ROOT / name).resolve()
    if target == BUILD_ROOT or BUILD_ROOT not in target.parents:
        raise ValueError(f"build target escapes {BUILD_ROOT}: {target}")
    if target.is_symlink():
        raise ValueError(f"refusing to remove a symlinked build directory: {target}")
    if not target.is_dir():
        raise ValueError(f"build directory does not exist: {target}")

    source = cmake_source_directory(target)
    if source is None:
        raise ValueError(f"missing CMakeCache.txt or CMAKE_HOME_DIRECTORY: {target}")
    if not same_path(source, REPOSITORY_ROOT):
        raise ValueError(f"CMake build belongs to {source}, not {REPOSITORY_ROOT}: {target}")
    return target


def known_build_names() -> list[str]:
    if not BUILD_ROOT.is_dir():
        return []
    names: list[str] = []
    for candidate in BUILD_ROOT.iterdir():
        if candidate.is_dir() and not candidate.is_symlink():
            source = cmake_source_directory(candidate)
            if source is not None and same_path(source, REPOSITORY_ROOT):
                names.append(candidate.name)
    return sorted(names)


def make_writable_and_retry(function, path: str, _error) -> None:
    os.chmod(path, stat.S_IWRITE)
    function(path)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Delete selected build/<name> CMake trees. The immutable dependency "
            "download cache under .cache/mcdev-deps/downloads is retained."
        ),
        epilog=(
            "examples:\n"
            "  python tools/scripts/clean_cmake_cache.py\n"
            "  python tools/scripts/clean_cmake_cache.py --dry-run x64-msvc-release\n"
            "  python tools/scripts/clean_cmake_cache.py x64-msvc-release\n"
            "  python tools/scripts/clean_cmake_cache.py --all"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "build_names",
        nargs="*",
        metavar="BUILD_NAME",
        help=f"build tree under build/ (default: {DEFAULT_BUILD_NAME})",
    )
    parser.add_argument("--all", action="store_true", help="clean every validated build tree")
    parser.add_argument("--dry-run", action="store_true", help="show validated targets without deleting them")
    arguments = parser.parse_args()
    if arguments.all and arguments.build_names:
        parser.error("BUILD_NAME and --all cannot be used together")
    if not arguments.all and not arguments.build_names:
        arguments.build_names = [DEFAULT_BUILD_NAME]
    return arguments


def main() -> int:
    arguments = parse_arguments()
    names = known_build_names() if arguments.all else arguments.build_names
    if not names:
        print("No validated CMake build trees were found.")
        return 0

    try:
        targets = [validate_build_name(name) for name in names]
    except ValueError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    for target in targets:
        if arguments.dry_run:
            print(f"Would remove: {target}")
        else:
            shutil.rmtree(target, onerror=make_writable_and_retry)
            print(f"Removed: {target}")

    action = "Would retain" if arguments.dry_run else "Retained"
    print(f"{action} shared dependency downloads: {DOWNLOAD_CACHE}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
