# AGENTS.md — Lotus Program Analysis Framework

## Project Overview

Lotus is a **program analysis, verification, and optimization framework** built on LLVM. It provides alias analysis, intermediate representations, dataflow analysis, abstract interpretation, bug checkers, etc.

- **Language**: C++17
- **Dependencies**: LLVM 14.x, Z3, CMake 3.16+
- **Docs**: https://zju-pl.github.io/lotus

## Repository Layout

```
lotus/
├── include/           # Public headers (mirrors lib structure)
│   ├── Alias/         # Alias analysis (DyckAA, AserPTA, LotusAA, SparrowAA, etc.)
│   ├── Analysis/      # Analysis utilities (NullPointer,CFG, etc.)
│   ├── CFL/           # CFL reachability
│   ├── Checker/       # Bug checkers (AE, Concurrency, FiTx, KINT, Pulse, Saber etc.)
│   ├── Concurrency/   # Concurrency analyses (MHP, lockset, MPI, OpenMP, kernel, CUDA, etc.)
│   ├── Dataflow/      # APA, IFDS/IDE, Mono, NPA, VASCO, WPDS
│   ├── IR/            # GSA, GVFG, ICFG, PDG, SSI, SVFG, vSSA, etc.
│   ├── Solvers/       # Datalog, EGraph, SMT
│   ├── Transform/     # LLVM bitcode transformations
│   ├── Utils/         # LLVM utilities, ThreadPool, formats, etc.
│   └── Verification/  # SIFA, CLAM, smarck, Seahorn, etc.
├── lib/               # Implementations (mirrors include)
├── tools/             # Command-line tools (alias, checker, verifier, ir, etc.)
├── tests/             # GTest-based tests (tests/unit/ mirrors subsystems)
├── benchmarks/        # Benchmark programs
├── third-party/       # CUDD, WPDS, spdlog
├── scripts/           # Python utilities
└── docs/              # Sphinx documentation (source/)
```

**Convention**: `include/` holds headers; `lib/` holds `.cpp` sources. Directory names match between them (e.g., `include/Alias/UnificationBased/DyckAA/`, `lib/Alias/UnificationBased/DyckAA/`).

## Build System

- **CMake**: Root `CMakeLists.txt` configures LLVM, Z3, optional Boost
- **Libraries**: Static libs prefixed `Canary*` (e.g., `CanaryDyckAA`, `CanaryPDG`) — legacy naming
- **Tools**: Binaries go to `build/bin/` (e.g., `lotus-alias-lotus-aa`, `lotus-check`, `clam`)
- **Tests**: Unit tests are organized under `tests/unit/`; shared unit-test CMake helpers live in `tests/unit/UnitTestHelpers.cmake`

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug   # Debug for assertions
make -j$(nproc)
make test
```

Custom LLVM path: `cmake .. -DLLVM_BUILD_PATH=/path/to/llvm/lib/cmake/llvm`

## Coding Conventions

| Element        | Style       | Example                    |
|----------------|------------|----------------------------|
| Classes        | CamelCase  | `NullCheckAnalysis`        |
| Functions      | camelCase  | `getPointsToSet`           |
| Variables      | snake_case | `points_to_set`            |
| Constants      | UPPER_CASE | `MAX_ITERATIONS`           |
| Member vars    | m_ or _    | `m_AnalysisMap`            |
| Indentation    | 2 spaces   | —                          |
| Line length    | ≤ 100 chars| —                          |
| Headers        | `#pragma once` or include guards | — |

**Namespaces**: Use `lotus::` for framework code (e.g., `lotus::sifa`). Some modules use `using namespace llvm` in `.cpp` files.

## Architecture

```
Tools (lotus-alias-aser-aa, lotus-alias-dyck-aa, lotus-check, clam, etc.)
    ↓
Analysis Applications (Checkers, Optimization, Verification, etc.)
    ↓
Core: Alias Analysis | IR  | Dataflow  Analysis | Abstract Interpretation
    ↓
LLVM (Module, Function, BasicBlock, Instruction) | Solvers
```


## Testing

- Tests live under `tests/unit/` and are grouped by subsystem (`Analysis`, `Checker`, `Concurrency`, `ControlFlow`, `DataFlow`, `Fuzzing`, `IR`, `Pointer`, `Solvers`, `TypeHierarchy`, `Utils`, `Verification`).
- Shared unit-test build helpers are defined in `tests/unit/UnitTestHelpers.cmake`, which is included by `tests/unit/CMakeLists.txt`.
- Add new tests with the subsystem-specific helpers from `tests/unit/UnitTestHelpers.cmake`, e.g. `add_lotus_analysis_test`, `add_lotus_concurrency_test`, `add_lotus_ir_test`, `add_lotus_pointer_test`, `add_lotus_verification_test`.
- Use `add_lotus_targeted_test(...)` only when no existing subsystem helper fits; keep the link set minimal and add subsystem-specific libraries explicitly.
- Shared test support targets include `lotus_test_utils` and `lotus_test_harness_utils`; prefer them over reintroducing large catch-all link bundles.
- Run all tests with `cd build && ctest --output-on-failure`, or build specific test targets with `cmake --build build --target <test_name>`.
