#!/usr/bin/env python3
"""Run a labeled CTest layer and enforce an LLVM line-coverage baseline."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


def run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
    print("+", " ".join(command), flush=True)
    return subprocess.run(command, check=True, text=True, **kwargs)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--source-dir", type=Path, required=True)
    parser.add_argument("--ctest", required=True)
    parser.add_argument("--profdata", required=True)
    parser.add_argument("--cov", required=True)
    parser.add_argument("--label", default="unit")
    parser.add_argument("--minimum", type=float, default=0.0)
    args = parser.parse_args()

    build_dir = args.build_dir.resolve()
    profile_dir = build_dir / "coverage" / args.label
    if profile_dir.exists():
        shutil.rmtree(profile_dir)
    profile_dir.mkdir(parents=True)

    environment = os.environ.copy()
    environment["LLVM_PROFILE_FILE"] = str(profile_dir / "%m-%p.profraw")
    run(
        [args.ctest, "--test-dir", str(build_dir), "-L", args.label,
         "--output-on-failure"],
        env=environment,
    )

    raw_profiles = sorted(profile_dir.glob("*.profraw"))
    if not raw_profiles:
        print("No LLVM raw profiles were produced.", file=sys.stderr)
        return 1
    merged_profile = profile_dir / "coverage.profdata"
    run([args.profdata, "merge", "-sparse", *map(str, raw_profiles),
         "-o", str(merged_profile)])

    binaries = sorted(
        path for path in (build_dir / "bin" / "tests").iterdir()
        if path.is_file() and os.access(path, os.X_OK)
    )
    if not binaries:
        print("No instrumented test binaries were found.", file=sys.stderr)
        return 1

    command = [args.cov, "report", str(binaries[0])]
    for binary in binaries[1:]:
        command.extend(["-object", str(binary)])
    common_options = [
        f"-instr-profile={merged_profile}",
        "-ignore-filename-regex=(/third-party/|/tests/)",
    ]
    command.extend(common_options)
    report = run(command, capture_output=True).stdout
    print(report, end="")
    (profile_dir / "summary.txt").write_text(report, encoding="utf-8")

    export_command = [args.cov, "export", str(binaries[0])]
    for binary in binaries[1:]:
        export_command.extend(["-object", str(binary)])
    export_command.extend([*common_options, "-summary-only"])
    coverage_json = run(export_command, capture_output=True).stdout
    total = float(json.loads(coverage_json)["data"][0]["totals"]["lines"]["percent"])
    if total < args.minimum:
        print(
            f"Line coverage {total:.2f}% is below the "
            f"{args.minimum:.2f}% baseline.",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
