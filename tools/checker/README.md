# Checker tools

This directory now builds a single checker frontend, `lotus-check`, on top of
`lib/Checker/` and related Lotus analysis infrastructure.

`lotus-check` supports:

- registry-backed generic/declarative checking
- native in-process checker engines such as `ae`, `kint`, `pulse`,
  `saber`, `fitx`, `concur`, `symex`, and `taint`

Each invocation selects exactly one engine with `--engine=<name>`. A native
engine may run several of its own checker kinds, while generic mode may run
several declarative checkers. Cross-engine orchestration such as running Pulse
and KINT in one invocation is not currently supported.

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

Binaries are emitted under `build/bin/`.

## Input format

All checker tools consume LLVM bitcode or textual LLVM IR.

```bash
clang -g -emit-llvm -c test.c -o test.bc
build/bin/lotus-check --engine=ae test.bc
```

Use `build/bin/lotus-check --help` or
`build/bin/lotus-check --engine=<name> --help` to inspect the full option set.
`build/bin/lotus-check --list-parameters` prints the global parameter set and
all engine-qualified parameter groups, similar to Z3's parameter-description
view.

## Engines

| Engine | Purpose | Notes |
| --- | --- | --- |
| `generic` | Generic checker driver | Resolves specs from `--generic.spec-dir`, `LOTUS_CHECKER_SPEC_DIR`, the installed data directory, or the source tree (in that order). |
| `kint` | Integer bug detector | Checks integer overflow, division by zero, bad shifts, array bounds, and dead branches. Runs all checks by default; `--checks` selects a subset. |
| `taint` | Interprocedural taint analysis | IFDS-based taint tracking with selectable alias analysis via `--taint.alias-analysis` and optional micro-benchmark evaluation mode. |
| `concur` | Concurrency checker | Detects races, deadlocks, atomicity issues, condvar misuse, lock mismatches, and OpenMP/MPI bugs. |
| `pulse` | Pulse-inspired bug finder | Biabductive analysis with optional SMT disabling via `--pulse.smt=off`; can emit JSON findings. |
| `fitx` | FiTx multi-checker driver | Runs typestate checks such as `double-free`, `double-lock`, `memory-leak`, `null-deref`, and `use-after-free`. |
| `saber` | Source-sink bug checker | Runs memory leak, double-free, and file-descriptor leak checks. Implemented by `tools/checker/lotus-check-saber.cpp`. |
| `ae` | Abstract-execution checker | Covers overflow, null dereference, use-after-free, invalid free, and memory leak detection. Implemented by `tools/checker/lotus-check-ae.cpp`. |
| `symex` | Symbolic-execution checker | Runs the `lib/SymbolicExecution` engine on GVFG/LotusAA and emits path-sensitive bug reports. Implemented by `tools/checker/lotus-check-symex.cpp`. |

## Common workflows

### Use the unified frontend

```bash
build/bin/lotus-check --list-checkers
build/bin/lotus-check --engine=generic --checks=forbidden.system test.bc
build/bin/lotus-check --engine=generic test.bc --checks=forbidden.system
build/bin/lotus-check --engine=ae test.bc --checks=all
build/bin/lotus-check --engine=concur test.bc --checks=data-race,deadlock

# Context-bounded MSli refinement with thread-aware MemorySSA
build/bin/lotus-check --engine=concur test.bc --checks=data-race \
  --concur.msli --concur.thread-context=2 \
  --concur.memory-partition=inter-disjoint
```

`--list-checkers` lists every check ID together with its engine, execution mode,
default status, category, and title. Native check IDs are passed to `--checks`
under the corresponding `--engine=<name>` value.

Parameters whose meaning belongs to one engine use a Z3-style qualified name,
`--<engine>.<parameter>`. Parameters with identical leaf names may therefore
have different engine-specific semantics without sharing one global setting.
Only parameters with the same cross-engine contract, such as `--checks`,
`--log-level`, `--analysis-stats`, and report options, remain unqualified.

### Run Kint on integer-heavy code

```bash
build/bin/lotus-check --engine=kint test.bc
```

### Run taint analysis with explicit sources and sinks

```bash
build/bin/lotus-check --engine=taint test.bc \
  --taint.alias-analysis=dyck \
  --taint.sources=recv,getenv \
  --taint.sinks=system,execve \
  --log-level=debug
```

### Run targeted bug checkers

```bash
build/bin/lotus-check --engine=saber test.bc --checks=all
build/bin/lotus-check --engine=ae test.bc --checks=all
build/bin/lotus-check --engine=symex test.bc --checks=null-deref,use-after-free
build/bin/lotus-check --engine=fitx test.bc --checks=use-after-free
build/bin/lotus-check --engine=concur test.bc --checks=data-race,deadlock
build/bin/lotus-check --engine=pulse test.bc --report-json=pulse.json
```

## Reporting

- Most checkers support shared report-output flags such as `--report-json` and
  `--report-sarif` through the common report manager.
- Several tools also support suppression files and confidence filtering through
  the same reporting layer.
- Successful analysis returns 0, `--fail-on-findings` returns 1 when filtered
  findings remain, and handled parameter/report I/O failures return 2.
- Pulse uses the same `--report-json`, `--report-sarif`,
  `--report-min-score`, and `--fail-on-findings` options as the other native
  checker engines.

## Declarative source/sink models

Name-only `sources`, `sinks`, and `sanitizers` remain supported. Sources model
the call return value, sinks match any argument, and sanitizers model a clean
return value. Specs that need out-parameters or an exact argument can add
structured models:

```yaml
source_models:
  - function: read
    selector: memory
    arg: 1
sink_models:
  - function: consume
    selector: argument
    arg: 0
sanitizer_models:
  - function: sanitize_buffer
    selector: memory
    arg: 0
```

Supported selectors are `return`, `argument` (or `arg`), and `memory`.
Sink models additionally support `any-argument`. The generic driver propagates
these facts through SSA expressions, casts, aggregates, simple memory, and
direct interprocedural argument/return flow. Calls through pointer casts are
resolved to their direct function; genuinely indirect calls still require a
points-to-backed engine such as the native `taint` checker.

## Related documentation

- `lib/Checker/README.md` describes the checker subsystems.
- `tools/alias/README.md` documents the alias-analysis drivers used by some
  checker pipelines.
