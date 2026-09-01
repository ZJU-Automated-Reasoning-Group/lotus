#!/usr/bin/env python3
"""Run Lotus and an external SVF CFL oracle over the same corpus."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time
from typing import Any


METRICS = (
    "NumOfEdges",
    "NumOfNodes",
    "SumEdges",
    "numOfChecks",
    "numOfIteration",
    "AnalysisTime",
)


def run(command: list[str], timeout: float, env: dict[str, str] | None = None) -> dict[str, Any]:
    started = time.perf_counter()
    try:
        process = subprocess.run(
            command,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=timeout,
            env=env,
            check=False,
        )
        return {
            "returncode": process.returncode,
            "wall_seconds": time.perf_counter() - started,
            "output": process.stdout,
            "timeout": False,
        }
    except subprocess.TimeoutExpired as error:
        output = error.stdout or ""
        if isinstance(output, bytes):
            output = output.decode(errors="replace")
        return {
            "returncode": None,
            "wall_seconds": time.perf_counter() - started,
            "output": output,
            "timeout": True,
        }


def lotus_result(
    binary: Path, input_path: Path, timeout: float, encoding: str, solver: str
) -> dict[str, Any]:
    result = run(
        [
            str(binary),
            "--solver",
            solver,
            "--encoding",
            encoding,
            "--check-annotations",
            "--json-stats",
            str(input_path),
        ],
        timeout,
    )
    stats = {}
    for line in reversed(result["output"].splitlines()):
        if line.startswith("{"):
            stats = json.loads(line)
            break
    result["stats"] = stats
    result["annotation_passes"] = result["output"].count("annotation=pass")
    result["annotation_failures"] = result["output"].count("annotation=fail")
    del result["output"]
    return result


def translated_annotations(source: Path, directory: Path) -> Path:
    if source.suffix != ".ll":
        return source
    text = source.read_text()
    text = text.replace("__aser_alias__", "MAYALIAS")
    text = text.replace("__aser_no_alias__", "NOALIAS")
    translated = directory / source.name
    translated.write_text(text)
    return translated


def parse_svf_metrics(output: str) -> dict[str, Any]:
    metrics: dict[str, Any] = {}
    plain = re.sub(r"\x1b\[[0-9;]*m", "", output)
    for name in METRICS:
        match = re.search(rf"^{re.escape(name)}\s+([^\s]+)", plain, re.MULTILINE)
        if not match:
            continue
        value = match.group(1)
        try:
            metrics[name] = float(value) if "." in value else int(value)
        except ValueError:
            metrics[name] = value
    metrics["annotation_passes"] = len(re.findall(r"SUCCESS\s*:", plain))
    metrics["annotation_failures"] = len(re.findall(r"FAILURE\s*:", plain))
    return metrics


def svf_result(
    binary: Path,
    grammar: Path,
    extapi: Path,
    input_path: Path,
    timeout: float,
    run_directory: Path,
) -> dict[str, Any]:
    env = os.environ.copy()
    env["DYLD_LIBRARY_PATH"] = "/usr/local/lib:" + env.get("DYLD_LIBRARY_PATH", "")
    result = run(
        [
            str(binary),
            f"-extapi={extapi}",
            f"-grammar={grammar}",
            "-alias-check",
            str(input_path),
        ],
        timeout,
        env,
    )
    result["stats"] = parse_svf_metrics(result["output"])
    del result["output"]
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lotus", required=True, type=Path)
    parser.add_argument("--svf", required=True, type=Path)
    parser.add_argument("--svf-grammar", required=True, type=Path)
    parser.add_argument("--svf-extapi", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--encoding", choices=("pag", "peg"), default="pag")
    parser.add_argument(
        "--lotus-solver", choices=("baseline", "pocr", "hybrid"), default="pocr"
    )
    parser.add_argument("inputs", nargs="+", type=Path)
    args = parser.parse_args()

    cases = []
    with tempfile.TemporaryDirectory(prefix="lotus-svf-cfl-") as temp:
        temp_path = Path(temp)
        for source in args.inputs:
            translated = translated_annotations(source, temp_path)
            lotus = lotus_result(
                args.lotus,
                source,
                args.timeout,
                args.encoding,
                args.lotus_solver,
            )
            svf = svf_result(
                args.svf,
                args.svf_grammar,
                args.svf_extapi,
                translated,
                args.timeout,
                temp_path,
            )
            lotus_total = lotus.get("stats", {}).get("annotation_total", 0)
            semantic_parity = (
                lotus.get("returncode") == 0
                and svf.get("returncode") == 0
                and lotus.get("annotation_failures") == 0
                and svf.get("stats", {}).get("annotation_failures", 0) == 0
                and lotus_total == svf.get("stats", {}).get("annotation_passes", 0)
            )
            cases.append(
                {
                    "input": str(source),
                    "semantic_parity": semantic_parity,
                    "lotus": lotus,
                    "svf": svf,
                }
            )

    annotated = [
        case
        for case in cases
        if case["lotus"].get("stats", {}).get("annotation_total", 0)
    ]
    successful_pairs = [
        case
        for case in cases
        if case["lotus"]["returncode"] == 0 and case["svf"]["returncode"] == 0
    ]
    report = {
        "schema": 1,
        "configuration": {
            "lotus_binary": str(args.lotus),
            "lotus_solver": args.lotus_solver,
            "encoding": args.encoding,
            "svf_binary": str(args.svf),
            "svf_grammar": str(args.svf_grammar),
            "timeout_seconds": args.timeout,
        },
        "cases": cases,
        "summary": {
            "case_count": len(cases),
            "timeouts": sum(case[tool]["timeout"] for case in cases for tool in ("lotus", "svf")),
            "lotus_failures": sum(case["lotus"]["returncode"] != 0 for case in cases),
            "svf_failures": sum(case["svf"]["returncode"] != 0 for case in cases),
            "successful_pairs": len(successful_pairs),
            "annotated_case_count": len(annotated),
            "semantic_parity_cases": sum(case["semantic_parity"] for case in annotated),
            "lotus_annotated_cases_passed": sum(
                case["lotus"].get("annotation_failures", 0) == 0
                for case in annotated
            ),
            "svf_annotated_cases_passed": sum(
                case["svf"].get("stats", {}).get("annotation_failures", 0) == 0
                for case in annotated
            ),
            "lotus_wall_seconds": sum(case["lotus"]["wall_seconds"] for case in cases),
            "svf_wall_seconds": sum(case["svf"]["wall_seconds"] for case in cases),
            "matched_lotus_wall_seconds": sum(
                case["lotus"]["wall_seconds"] for case in successful_pairs
            ),
            "matched_svf_wall_seconds": sum(
                case["svf"]["wall_seconds"] for case in successful_pairs
            ),
        },
    }
    args.output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    print(json.dumps(report["summary"], sort_keys=True))
    return 0 if all(case["semantic_parity"] for case in annotated) else 1


if __name__ == "__main__":
    raise SystemExit(main())
