#!/usr/bin/env python3
"""Run reproducible baseline/compact control-dependence experiments.

The C++ driver executes one primitive algorithm once. This script owns all
experiment policy: input discovery, warmups, randomized run ordering,
repetition, aggregation, output checks, and report generation.
"""

from __future__ import annotations

import argparse
import csv
import io
import json
import math
import os
import platform
import random
import statistics
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


@dataclass(frozen=True)
class Experiment:
    baseline: str
    compact: str
    visit_pairs: bool = False
    result_field: str = "dependencies"


EXPERIMENTS: dict[str, Experiment] = {
    "ntscd": Experiment("ntscd2", "ntscd-compact"),
    "dod-preprocess": Experiment(
        "dod", "dod-compact", result_field="bicliques"
    ),
    "dod-enumerate": Experiment(
        "dod", "dod-compact", visit_pairs=True, result_field="dod_pairs"
    ),
    "combined": Experiment("dod-ntscd", "dod-ntscd-compact"),
    "closure": Experiment(
        "strong-closure", "compact-closure", result_field="closure_size"
    ),
}

TIMING_FIELDS = (
    "analysis_ns",
    "inevitability_ns",
    "ntscd_ns",
    "dod_ns",
    "pair_visit_ns",
    "closure_ns",
)
COUNT_FIELDS = (
    "functions",
    "nodes",
    "edges",
    "decisions",
    "dependencies",
    "bicliques",
    "incidences",
    "dod_pairs",
    "closure_size",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "inputs",
        nargs="+",
        help="LLVM .bc/.ll files or directories searched recursively",
    )
    parser.add_argument(
        "--tool",
        default="build-release/bin/lotus-ir-control-dependence",
        help="path to the Release control-dependence driver",
    )
    parser.add_argument(
        "--experiments",
        default="ntscd,dod-preprocess,dod-enumerate,combined",
        help="comma-separated names: " + ",".join(EXPERIMENTS),
    )
    parser.add_argument("--repeat", type=int, default=20)
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--seed", type=int, default=0xC0D0D)
    parser.add_argument("--function", default="")
    parser.add_argument(
        "--seed-index",
        type=int,
        action="append",
        default=[],
        help="extra closure seed block index; repeatable",
    )
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--output-dir", default="control-dependence-results")
    parser.add_argument(
        "--keep-going",
        action="store_true",
        help="record failed commands and continue with other inputs",
    )
    args = parser.parse_args()
    if args.repeat <= 0:
        parser.error("--repeat must be greater than zero")
    if args.warmup < 0:
        parser.error("--warmup cannot be negative")
    if args.timeout <= 0:
        parser.error("--timeout must be greater than zero")
    if args.seed_index and not args.function:
        parser.error("--seed-index requires --function because block indices are per-function")
    return args


def discover_inputs(arguments: Sequence[str]) -> list[Path]:
    files: set[Path] = set()
    for argument in arguments:
        path = Path(argument)
        if path.is_file():
            if path.suffix not in {".bc", ".ll"}:
                raise ValueError(f"unsupported input suffix: {path}")
            files.add(path.resolve())
        elif path.is_dir():
            for suffix in ("*.bc", "*.ll"):
                files.update(candidate.resolve() for candidate in path.rglob(suffix))
        else:
            raise FileNotFoundError(argument)
    if not files:
        raise ValueError("no .bc or .ll inputs found")
    return sorted(files)


def selected_experiments(value: str) -> list[str]:
    names = [name.strip() for name in value.split(",") if name.strip()]
    unknown = [name for name in names if name not in EXPERIMENTS]
    if unknown:
        raise ValueError("unknown experiments: " + ", ".join(unknown))
    if not names:
        raise ValueError("no experiments selected")
    return list(dict.fromkeys(names))


def aggregate_driver_rows(rows: list[dict[str, str]]) -> dict[str, int | str]:
    if not rows:
        raise ValueError("driver produced no function rows")
    result: dict[str, int | str] = {"algorithm": rows[0]["algorithm"]}
    result["functions"] = len(rows)
    for field in COUNT_FIELDS[1:] + TIMING_FIELDS:
        result[field] = sum(int(row[field]) for row in rows)
    return result


def run_driver(
    tool: Path,
    input_path: Path,
    algorithm: str,
    experiment: Experiment,
    function: str,
    seed_indices: Sequence[int],
    timeout: float,
) -> tuple[dict[str, int | str], int, list[str]]:
    command = [
        str(tool),
        str(input_path),
        f"--algorithm={algorithm}",
        "--format=csv",
    ]
    if experiment.visit_pairs:
        command.append("--visit-pairs")
    if function:
        command.append(f"--function={function}")
    if algorithm in {"strong-closure", "compact-closure"}:
        command.extend(f"--seed-index={index}" for index in seed_indices)

    started = time.perf_counter_ns()
    completed = subprocess.run(
        command,
        text=True,
        capture_output=True,
        timeout=timeout,
        check=False,
    )
    wall_ns = time.perf_counter_ns() - started
    if completed.returncode != 0:
        message = completed.stderr.strip() or completed.stdout.strip()
        raise RuntimeError(
            f"command failed ({completed.returncode}): {' '.join(command)}\n{message}"
        )
    rows = list(csv.DictReader(io.StringIO(completed.stdout)))
    return aggregate_driver_rows(rows), wall_ns, command


def percentile(values: Sequence[int], fraction: float) -> float:
    ordered = sorted(values)
    if len(ordered) == 1:
        return float(ordered[0])
    position = fraction * (len(ordered) - 1)
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return float(ordered[lower])
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def summarize_samples(samples: Sequence[dict[str, object]]) -> dict[str, float | int]:
    values = [int(sample["analysis_ns"]) for sample in samples]
    q1 = percentile(values, 0.25)
    q3 = percentile(values, 0.75)
    return {
        "runs": len(values),
        "median_ns": statistics.median(values),
        "mean_ns": statistics.fmean(values),
        "stdev_ns": statistics.stdev(values) if len(values) > 1 else 0.0,
        "min_ns": min(values),
        "max_ns": max(values),
        "q1_ns": q1,
        "q3_ns": q3,
        "iqr_ns": q3 - q1,
    }


def output_count(sample: dict[str, object], experiment: Experiment) -> int:
    return int(sample[experiment.result_field])


def write_csv(path: Path, rows: Sequence[dict[str, object]]) -> None:
    if not rows:
        return
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    args = parse_args()
    try:
        inputs = discover_inputs(args.inputs)
        experiment_names = selected_experiments(args.experiments)
    except (OSError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    tool = Path(args.tool).resolve()
    if not tool.is_file() or not os.access(tool, os.X_OK):
        print(f"error: tool is not executable: {tool}", file=sys.stderr)
        return 2
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    rng = random.Random(args.seed)
    raw_rows: list[dict[str, object]] = []
    failures: list[dict[str, object]] = []

    for input_path in inputs:
        for experiment_name in experiment_names:
            experiment = EXPERIMENTS[experiment_name]
            algorithms = (experiment.baseline, experiment.compact)

            # Warm each implementation equally; warmup output is discarded.
            for _ in range(args.warmup):
                warmup_order = list(algorithms)
                rng.shuffle(warmup_order)
                for algorithm in warmup_order:
                    try:
                        run_driver(
                            tool,
                            input_path,
                            algorithm,
                            experiment,
                            args.function,
                            args.seed_index,
                            args.timeout,
                        )
                    except (RuntimeError, subprocess.TimeoutExpired) as error:
                        if not args.keep_going:
                            raise
                        failures.append(
                            {
                                "input": str(input_path),
                                "experiment": experiment_name,
                                "algorithm": algorithm,
                                "stage": "warmup",
                                "error": str(error),
                            }
                        )

            schedule = [
                (run_index, algorithm)
                for run_index in range(args.repeat)
                for algorithm in algorithms
            ]
            rng.shuffle(schedule)
            for run_index, algorithm in schedule:
                try:
                    aggregate, wall_ns, command = run_driver(
                        tool,
                        input_path,
                        algorithm,
                        experiment,
                        args.function,
                        args.seed_index,
                        args.timeout,
                    )
                except (RuntimeError, subprocess.TimeoutExpired) as error:
                    if not args.keep_going:
                        raise
                    failures.append(
                        {
                            "input": str(input_path),
                            "experiment": experiment_name,
                            "algorithm": algorithm,
                            "run": run_index,
                            "stage": "measure",
                            "error": str(error),
                        }
                    )
                    continue
                raw_rows.append(
                    {
                        "input": str(input_path),
                        "benchmark": input_path.name,
                        "experiment": experiment_name,
                        "algorithm": algorithm,
                        "implementation": (
                            "baseline" if algorithm == experiment.baseline else "compact"
                        ),
                        "run": run_index,
                        **aggregate,
                        "wall_ns": wall_ns,
                        "command": " ".join(command),
                    }
                )
                print(
                    f"{input_path.name}: {experiment_name} {algorithm} "
                    f"run {run_index + 1}/{args.repeat}",
                    file=sys.stderr,
                )

    summary_rows: list[dict[str, object]] = []
    for input_path in inputs:
        for experiment_name in experiment_names:
            experiment = EXPERIMENTS[experiment_name]
            baseline = [
                row
                for row in raw_rows
                if row["input"] == str(input_path)
                and row["experiment"] == experiment_name
                and row["implementation"] == "baseline"
            ]
            compact = [
                row
                for row in raw_rows
                if row["input"] == str(input_path)
                and row["experiment"] == experiment_name
                and row["implementation"] == "compact"
            ]
            if not baseline or not compact:
                continue
            baseline_stats = summarize_samples(baseline)
            compact_stats = summarize_samples(compact)
            baseline_output = {output_count(row, experiment) for row in baseline}
            compact_output = {output_count(row, experiment) for row in compact}
            outputs_match = (
                len(baseline_output) == 1
                and len(compact_output) == 1
                and baseline_output == compact_output
            )
            speedup = float(baseline_stats["median_ns"]) / float(
                compact_stats["median_ns"]
            )
            first = baseline[0]
            summary_rows.append(
                {
                    "input": str(input_path),
                    "benchmark": input_path.name,
                    "experiment": experiment_name,
                    "baseline_algorithm": experiment.baseline,
                    "compact_algorithm": experiment.compact,
                    "functions": first["functions"],
                    "nodes": first["nodes"],
                    "edges": first["edges"],
                    "decisions": first["decisions"],
                    "output_field": experiment.result_field,
                    "baseline_output": next(iter(baseline_output)),
                    "compact_output": next(iter(compact_output)),
                    "outputs_match": outputs_match,
                    "baseline_median_ns": baseline_stats["median_ns"],
                    "compact_median_ns": compact_stats["median_ns"],
                    "speedup": speedup,
                    "baseline_iqr_ns": baseline_stats["iqr_ns"],
                    "compact_iqr_ns": compact_stats["iqr_ns"],
                    "baseline_min_ns": baseline_stats["min_ns"],
                    "compact_min_ns": compact_stats["min_ns"],
                }
            )

    write_csv(output_dir / "raw.csv", raw_rows)
    write_csv(output_dir / "summary.csv", summary_rows)
    metadata = {
        "created_unix": time.time(),
        "tool": str(tool),
        "inputs": [str(path) for path in inputs],
        "experiments": experiment_names,
        "repeat": args.repeat,
        "warmup": args.warmup,
        "seed": args.seed,
        "function": args.function,
        "seed_indices": args.seed_index,
        "python": sys.version,
        "platform": platform.platform(),
        "failures": failures,
    }
    (output_dir / "metadata.json").write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n"
    )

    print(
        "benchmark,experiment,baseline_ms,compact_ms,speedup,outputs_match"
    )
    for row in summary_rows:
        print(
            f"{row['benchmark']},{row['experiment']},"
            f"{float(row['baseline_median_ns']) / 1e6:.6f},"
            f"{float(row['compact_median_ns']) / 1e6:.6f},"
            f"{float(row['speedup']):.3f},{row['outputs_match']}"
        )
    if failures:
        print(f"warning: {len(failures)} command(s) failed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
