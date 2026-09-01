#!/usr/bin/env python3
"""Fail when a tests/unit C++ source is absent from unit-test CMake files."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


CPP_TOKEN = re.compile(r"(?P<path>[A-Za-z0-9_./+-]+\.cpp)\b")
GTEST_MACRO = re.compile(r"\bTEST(?:_F|_P)?\s*\(")


def registered_sources(unit_root: Path) -> set[Path]:
    registered: set[Path] = set()
    for cmake_file in unit_root.rglob("CMakeLists.txt"):
        contents = cmake_file.read_text(encoding="utf-8")
        for match in CPP_TOKEN.finditer(contents):
            candidate = (cmake_file.parent / match.group("path")).resolve()
            if candidate.is_relative_to(unit_root) and candidate.is_file():
                registered.add(candidate)
    return registered


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "unit_root",
        nargs="?",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "tests" / "unit",
    )
    args = parser.parse_args()

    unit_root = args.unit_root.resolve()
    existing = {
        source.resolve()
        for source in unit_root.rglob("*.cpp")
        if GTEST_MACRO.search(source.read_text(encoding="utf-8", errors="ignore"))
    }
    missing = sorted(existing - registered_sources(unit_root))
    if missing:
        print("Unregistered tests/unit C++ sources:", file=sys.stderr)
        for source in missing:
            print(f"  {source.relative_to(unit_root)}", file=sys.stderr)
        return 1

    print(f"All {len(existing)} GTest C++ sources are registered.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
