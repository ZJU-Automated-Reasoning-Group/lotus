#!/usr/bin/env python3
"""Run the paper's reproducible RQ1/RQ2 control-dependence experiments.

The C++ driver executes one primitive algorithm once. This script owns all
experiment policy: input discovery, warmups, randomized run ordering,
repetition, aggregation, output checks, and report generation.

RQ1 compares the full algorithms with the state of the art for DOD enumeration
and rooted strong control closure. RQ2 compares the full algorithms with the
Exact-Set and Eager-Pairs ablations.

Paper-to-driver map
-------------------

``rq1-enumeration``
    SOTA-Enumerate vs. Full-Enumerate. Both algorithms visit all K exact DOD
    triples, so ``analysis_ns`` is the like-for-like end-to-end enumeration
    time used in RQ1.
``rq1-closure``
    SOTA-Closure vs. Full-Closure. Both receive the same closure seed and return
    the same rooted strong control-closure set.
``rq2-cardinality``
    Full-Build vs. Exact-Set. Exact-Set changes only SCC propagation: it carries
    complete first-hit sets instead of the capped 0/1/many abstraction.
``rq2-consumption``
    Full-Closure vs. Eager-Pairs. Eager-Pairs uses the same compact construction
    but expands and indexes all K cross-side pairs before closure.

The driver reports ``K`` as ``dod_pairs`` and the compact incidence count ``C``
as ``incidences``. Their ratio K/C is the paper's representation-compression
metric. ``analysis_ns`` excludes IR parsing and CFG extraction; ``wall_ns`` is
also retained in raw.csv when whole-process timing is useful. Peak RSS is a
whole-process maximum reported by the driver.

Outputs
-------

``raw.csv`` contains every measured repetition. ``summary.csv`` contains one
paired aggregate per input and experiment. ``metadata.json`` records the
machine, seed, command configuration, and failures needed for reproduction.
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
    """One paired comparison corresponding to a paper result.

    ``reference`` and ``candidate`` name C++ driver algorithms; their labels are
    the names printed in the paper. For RQ1 the reference is SOTA and the
    candidate is Full. For RQ2 the reference is Full and the candidate is the
    ablation, so candidate/reference is the ablation overhead.

    ``visit_pairs`` requests actual traversal of every DOD pair rather than
    compact construction alone. ``result_field`` selects the cardinality that
    must agree across the pair; a fingerprint is checked as well.
    """

    rq: str
    reference: str
    candidate: str
    reference_label: str
    candidate_label: str
    visit_pairs: bool = False
    result_field: str = "dependencies"


# RQ1: external, identical-output comparisons against the state of the art.
EXPERIMENTS: dict[str, Experiment] = {
    "rq1-enumeration": Experiment(
        "RQ1",
        "dod",
        "dod-compact",
        "SOTA-Enumerate",
        "Full-Enumerate",
        visit_pairs=True,
        result_field="dod_pairs",
    ),
    "rq1-closure": Experiment(
        "RQ1",
        "strong-closure",
        "compact-closure",
        "SOTA-Closure",
        "Full-Closure",
        result_field="closure_size",
    ),
    # RQ2: internal ablations. These intentionally put Full in the reference
    # column so candidate_over_reference_* directly denotes ablation overhead.
    "rq2-cardinality": Experiment(
        "RQ2",
        "dod-compact",
        "dod-compact-exact-set",
        "Full-Build",
        "Exact-Set",
        result_field="bicliques",
    ),
    "rq2-consumption": Experiment(
        "RQ2",
        "compact-closure",
        "compact-closure-eager-pairs",
        "Full-Closure",
        "Eager-Pairs",
        result_field="closure_size",
    ),
}

# Per-function times are additive when a bitcode file contains many functions.
# The driver measures each phase independently; analysis_ns covers their full
# algorithm path and is the primary paper metric.
TIMING_FIELDS = (
    "analysis_ns",
    "inevitability_ns",
    "ntscd_ns",
    "dod_ns",
    "pair_visit_ns",
    "closure_ns",
)
# Structural counts are summed over functions. In particular, dod_pairs is K
# and incidences is C = sum_p (|L_p| + |R_p|).
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
    """Parse the reproducible experiment policy shared by all comparisons."""

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
        default=",".join(EXPERIMENTS),
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
        help=(
            "extra closure seed block index; repeatable; the function entry "
            "is always included by the driver"
        ),
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
        parser.error(
            "--seed-index requires --function because block indices are per-function"
        )
    return args


def discover_inputs(arguments: Sequence[str]) -> list[Path]:
    """Resolve files and recursively discover LLVM inputs deterministically."""

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
    """Validate the requested paper experiments while preserving their order."""

    names = [name.strip() for name in value.split(",") if name.strip()]
    unknown = [name for name in names if name not in EXPERIMENTS]
    if unknown:
        raise ValueError("unknown experiments: " + ", ".join(unknown))
    if not names:
        raise ValueError("no experiments selected")
    return list(dict.fromkeys(names))


def aggregate_driver_rows(rows: list[dict[str, str]]) -> dict[str, int | str]:
    """Aggregate the driver's per-function rows into one bitcode-input row.

    Counts and phase times add across functions. Peak RSS is a process-wide
    maximum and therefore must not be summed. Function fingerprints are
    combined modulo 2^64; paired comparisons require both this fingerprint and
    the selected output cardinality to match.
    """

    if not rows:
        raise ValueError("driver produced no function rows")
    result: dict[str, int | str] = {"algorithm": rows[0]["algorithm"]}
    result["functions"] = len(rows)
    for field in COUNT_FIELDS[1:] + TIMING_FIELDS:
        result[field] = sum(int(row[field]) for row in rows)
    result["peak_rss_kb"] = max(int(row["peak_rss_kb"]) for row in rows)
    result["result_fingerprint"] = sum(
        int(row["result_fingerprint"]) for row in rows
    ) & ((1 << 64) - 1)
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
    """Execute one algorithm once and retain both analysis and process timing.

    The C++ driver's ``analysis_ns`` is the primary evaluation measurement. The
    Python-side ``wall_ns`` additionally includes process startup, IR parsing,
    CFG extraction, CSV serialization, and shutdown and is kept in raw.csv.
    """

    command = [
        str(tool),
        str(input_path),
        f"--algorithm={algorithm}",
        "--format=csv",
    ]
    if experiment.visit_pairs:
        # RQ1 enumeration must pay for visiting all K triples on both sides.
        command.append("--visit-pairs")
    if function:
        command.append(f"--function={function}")
    if algorithm in {
        "strong-closure",
        "compact-closure",
        "compact-closure-eager-pairs",
    }:
        # Forward identical seeds to all closure implementations, including
        # Eager-Pairs. The driver adds the distinguished entry automatically.
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
    """Return a linearly interpolated sample percentile."""

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


def summarize_samples(
    samples: Sequence[dict[str, object]], field: str
) -> dict[str, float | int]:
    """Summarize repetitions for a time or memory field."""

    values = [int(sample[field]) for sample in samples]
    q1 = percentile(values, 0.25)
    q3 = percentile(values, 0.75)
    return {
        "runs": len(values),
        "median": statistics.median(values),
        "mean": statistics.fmean(values),
        "stdev": statistics.stdev(values) if len(values) > 1 else 0.0,
        "min": min(values),
        "max": max(values),
        "q1": q1,
        "q3": q3,
        "iqr": q3 - q1,
    }


def output_count(sample: dict[str, object], experiment: Experiment) -> int:
    """Read the experiment-specific result cardinality used for pairing."""

    return int(sample[experiment.result_field])


def write_csv(path: Path, rows: Sequence[dict[str, object]]) -> None:
    """Write a stable rectangular report, skipping empty result sets."""

    if not rows:
        return
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def main() -> int:
    # Phase 1: resolve the artifact configuration before creating result files.
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

    # Phase 2: collect paired samples. Each repetition contains exactly one run
    # of each algorithm; the global shuffle mitigates ordering and thermal bias.
    for input_path in inputs:
        for experiment_name in experiment_names:
            experiment = EXPERIMENTS[experiment_name]
            algorithms = (experiment.reference, experiment.candidate)

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
                            "reference"
                            if algorithm == experiment.reference
                            else "candidate"
                        ),
                        "run": run_index,
                        **aggregate,
                        # Retained for diagnostics; paper ratios use analysis_ns.
                        "wall_ns": wall_ns,
                        "command": " ".join(command),
                    }
                )
                print(
                    f"{input_path.name}: {experiment_name} {algorithm} "
                    f"run {run_index + 1}/{args.repeat}",
                    file=sys.stderr,
                )

    # Phase 3: pair outputs and compute the paper-facing aggregates. Medians are
    # used for time and peak RSS; IQR records run-to-run variability.
    summary_rows: list[dict[str, object]] = []
    for input_path in inputs:
        for experiment_name in experiment_names:
            experiment = EXPERIMENTS[experiment_name]
            reference = [
                row
                for row in raw_rows
                if row["input"] == str(input_path)
                and row["experiment"] == experiment_name
                and row["implementation"] == "reference"
            ]
            candidate = [
                row
                for row in raw_rows
                if row["input"] == str(input_path)
                and row["experiment"] == experiment_name
                and row["implementation"] == "candidate"
            ]
            if not reference or not candidate:
                continue
            reference_stats = summarize_samples(reference, "analysis_ns")
            candidate_stats = summarize_samples(candidate, "analysis_ns")
            reference_memory = summarize_samples(reference, "peak_rss_kb")
            candidate_memory = summarize_samples(candidate, "peak_rss_kb")
            reference_output = {output_count(row, experiment) for row in reference}
            candidate_output = {output_count(row, experiment) for row in candidate}
            reference_fingerprint = {
                int(row["result_fingerprint"]) for row in reference
            }
            candidate_fingerprint = {
                int(row["result_fingerprint"]) for row in candidate
            }
            outputs_match = (
                len(reference_output) == 1
                and len(candidate_output) == 1
                and reference_output == candidate_output
                and len(reference_fingerprint) == 1
                and len(candidate_fingerprint) == 1
                and reference_fingerprint == candidate_fingerprint
            )
            # RQ1 reads reference/candidate as SOTA/Full speedup. RQ2 reads
            # candidate/reference as the cost of disabling one design choice.
            reference_over_candidate = float(reference_stats["median"]) / float(
                candidate_stats["median"]
            )
            candidate_over_reference = float(candidate_stats["median"]) / float(
                reference_stats["median"]
            )
            memory_candidate_over_reference = float(candidate_memory["median"]) / float(
                reference_memory["median"]
            )
            first = reference[0]
            first_candidate = candidate[0]
            summary_rows.append(
                {
                    "input": str(input_path),
                    "benchmark": input_path.name,
                    "rq": experiment.rq,
                    "experiment": experiment_name,
                    "reference_label": experiment.reference_label,
                    "candidate_label": experiment.candidate_label,
                    "reference_algorithm": experiment.reference,
                    "candidate_algorithm": experiment.candidate,
                    "functions": first["functions"],
                    "nodes": first["nodes"],
                    "edges": first["edges"],
                    "decisions": first["decisions"],
                    "reference_bicliques": first["bicliques"],
                    "candidate_bicliques": first_candidate["bicliques"],
                    "reference_incidences": first["incidences"],
                    "candidate_incidences": first_candidate["incidences"],
                    "reference_dod_pairs": first["dod_pairs"],
                    "candidate_dod_pairs": first_candidate["dod_pairs"],
                    "output_field": experiment.result_field,
                    "reference_output": next(iter(reference_output)),
                    "candidate_output": next(iter(candidate_output)),
                    "reference_fingerprint": next(iter(reference_fingerprint)),
                    "candidate_fingerprint": next(iter(candidate_fingerprint)),
                    "outputs_match": outputs_match,
                    "reference_median_ns": reference_stats["median"],
                    "candidate_median_ns": candidate_stats["median"],
                    "reference_over_candidate_time": reference_over_candidate,
                    "candidate_over_reference_time": candidate_over_reference,
                    "reference_iqr_ns": reference_stats["iqr"],
                    "candidate_iqr_ns": candidate_stats["iqr"],
                    "reference_median_peak_rss_kb": reference_memory["median"],
                    "candidate_median_peak_rss_kb": candidate_memory["median"],
                    "candidate_over_reference_memory": memory_candidate_over_reference,
                }
            )

    # Phase 4: emit the artifact. raw.csv permits re-aggregation; summary.csv is
    # ready for paper tables/plots; metadata.json captures reproducibility data.
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

    # A compact stdout table makes smoke runs readable without opening the CSV.
    print(
        "benchmark,rq,experiment,reference,candidate,reference_ms,"
        "candidate_ms,reference_over_candidate,candidate_over_reference,"
        "outputs_match"
    )
    for row in summary_rows:
        print(
            f"{row['benchmark']},{row['rq']},{row['experiment']},"
            f"{row['reference_label']},{row['candidate_label']},"
            f"{float(row['reference_median_ns']) / 1e6:.6f},"
            f"{float(row['candidate_median_ns']) / 1e6:.6f},"
            f"{float(row['reference_over_candidate_time']):.3f},"
            f"{float(row['candidate_over_reference_time']):.3f},"
            f"{row['outputs_match']}"
        )
    if failures:
        print(f"warning: {len(failures)} command(s) failed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
