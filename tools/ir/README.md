# IR tools

This directory contains command-line frontends for LLVM-based intermediate
representations built in `lib/IR/`.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

The current IR tool binary is emitted under `build/bin/`.

## Tools

| Tool | Purpose | Notes |
| --- | --- | --- |
| `lotus-ir-pdg-query` | Query the Program Dependence Graph | Implemented by `tools/ir/lotus-ir-pdg-query.cpp`; supports Cypher-style queries, slicing, chopping, shortest paths, summaries, resource-flow queries, and multiple output formats. |
| `lotus-ir-control-dependence` | Run control-dependence experiments | Separates baseline and compact NTSCD/DOD timing, biclique statistics, exact pair enumeration, closure, and consistency checking; emits text, JSON, or CSV. |

## Typical usage

`lotus-ir-pdg-query` consumes LLVM bitcode or textual LLVM IR:

```bash
clang -emit-llvm -c test.c -o test.bc
build/bin/lotus-ir-pdg-query test.bc --query "MATCH (n) RETURN n LIMIT 5"
```

Useful options include:

- `--query` / `--query-file` to execute Cypher queries.
- `--interactive` to start an interactive query session.
- `--analysis` to run built-in PDG analyses such as `slice-forward`,
  `slice-backward`, `chop`, `shortest-path`, `impact`, or `resource-flow`.
- `--format=text|json|dot` to control output formatting.
- `--property-file` with `--direction` for property-driven slicing.
- `--edge-preset` and `--context-sensitive` to tune traversal behavior.

### Control-dependence experiments

The standalone driver runs exactly one algorithm per invocation and excludes
LLVM parsing/graph construction from `analysis_ns`. Experiment scripts are
responsible for repetitions, warmups, aggregation, and baseline/compact
pairing.

`scripts/evaluate_control_dependence.py` provides that orchestration. It
randomizes baseline/compact run order, performs warmups and repetitions, checks
output counts, and writes `raw.csv`, `summary.csv`, and `metadata.json`.

```bash
# DOD preprocessing only (run as separate script samples).
build/bin/lotus-ir-control-dependence test.bc --algorithm=dod --format=csv
build/bin/lotus-ir-control-dependence test.bc --algorithm=dod-compact --format=csv

# End-to-end preprocessing plus traversal of exactly K pairs. Individual
# pairs are never printed or stored; the callback only increments dod_pairs.
build/bin/lotus-ir-control-dependence test.bc \
  --algorithm=dod --visit-pairs --format=csv
build/bin/lotus-ir-control-dependence test.bc \
  --algorithm=dod-compact --visit-pairs --format=csv

# Closure comparison. Function entry is always in the seed.
build/bin/lotus-ir-control-dependence test.bc \
  --algorithm=strong-closure --seed-index=3 --format=json
build/bin/lotus-ir-control-dependence test.bc \
  --algorithm=compact-closure --seed-index=3 --format=json

# Reproducible multi-input evaluation (use the Release driver).
scripts/evaluate_control_dependence.py benchmarks/spec2006 \
  --tool build-release/bin/lotus-ir-control-dependence \
  --experiments ntscd,dod-preprocess,dod-enumerate,combined \
  --warmup 3 --repeat 20 --output-dir results/control-dependence
```

Relevant options:

- `--algorithm=<name>` selects one primitive algorithm operation.
- `--visit-pairs` is valid only for `dod` and `dod-compact`; it traverses exact
  pairs through the same allocation-free counting callback and performs no
  per-pair output.
- `--function=<name>` restricts the experiment to one function.
- `--seed-index=N` adds closure seeds; entry is included automatically.
- `--format=text|json|csv` selects stable machine-readable output.

## Examples

```bash
# Run a single query
build/bin/lotus-ir-pdg-query test.bc --query "MATCH (f:Function) RETURN f.name"

# Compute a backward slice from criteria selected by a query
build/bin/lotus-ir-pdg-query test.bc \
  --analysis=slice-backward \
  --criteria-query "MATCH (n {name:'x'}) RETURN n"

# Dump JSON output for scripting
build/bin/lotus-ir-pdg-query test.bc --query-file tools/ir/examples/dataflow.cypher --format=json
```

## Related documentation

- Query examples live in `tools/ir/examples/README.md` and `tools/ir/examples/`.
- PDG implementation details live in `lib/IR/PDG/README.md`.
