# AGENTS.md — Lotus Program Analysis Framework

## Project Overview

Lotus is a **program analysis, verification, and optimization framework** built on LLVM. It provides alias analysis, intermediate representations, dataflow analysis, constraint solvers, abstract interpretation, and bug checkers.

- **Language**: C++14
- **Dependencies**: LLVM 14.x, Z3, CMake 3.10+
- **Docs**: https://zju-pl.github.io/lotus

## Repository Layout

```
lotus/
├── include/           # Public headers (mirrors lib structure)
│   ├── Alias/         # Alias analysis (DyckAA, AserPTA, LotusAA, SparrowAA, etc.)
│   ├── Analysis/      # Analysis utilities (NullPointer, Concurrency, CFG, etc.)
│   ├── CFL/           # CFL reachability
│   ├── Checker/       # Bug checkers (Concurrency, FiTx, KINT, Pulse, etc.)
│   ├── Dataflow/      # APA, IFDS/IDE, Mono, NPA, WPDS
│   ├── IR/            # GSA, ICFG, MemorySSA, PDG, SSI, SVFG, vSSA
│   ├── Solvers/       # SMT
│   ├── Transform/     # LLVM bitcode transformations
│   ├── Utils/         # LLVM utilities, ThreadPool, formats, etc.
│   └── Verification/  # SIFA, CLAM, SymbolicAbstraction, Seahorn
├── lib/               # Implementations (mirrors include)
├── tools/             # Command-line tools (alias, checker, verifier, ir, etc.)
├── tests/             # GTest-based tests (tests/unit/ mirrors subsystems)
├── benchmarks/        # Benchmark programs
├── third-party/       # CUDD, WPDS, spdlog
├── scripts/           # Python and build utilities
└── docs/              # Sphinx documentation (source/)
```

**Convention**: `include/` holds headers; `lib/` holds `.cpp` sources. Directory names match between them (e.g., `include/Alias/DyckAA/`, `lib/Alias/DyckAA/`).

## Build System

- **CMake**: Root `CMakeLists.txt` configures LLVM, Z3, optional Boost/CLAM/SeaHorn
- **Libraries**: Static libs prefixed `Canary*` (e.g., `CanaryDyckAA`, `CanaryPDG`) — legacy naming
- **Tools**: Binaries go to `build/bin/` (e.g., `lotus-aa`, `lotus-kint`, `clam`)
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
Tools (aser-aa, dyck-aa, lotus-kint, clam, etc.)
    ↓
Analysis Applications (Checkers, Optimization, Verification, etc.)
    ↓
Core: Alias Analysis | IR  | Dataflow  Analysis | Abstract Interpretation
    ↓
LLVM (Module, Function, BasicBlock, Instruction) | Solvers
```

## Adding New Code

### New Alias Analysis

1. Create `include/Alias/MyAA/` and `lib/Alias/MyAA/`
2. Implement `ModulePass` (or `FunctionPass`) with `runOnModule`/`runOnFunction`
3. Optionally inherit `llvm::AliasAnalysis` and implement `alias()`, `getPointsToSet`
4. Add `CMakeLists.txt` under `lib/Alias/MyAA/` and `add_subdirectory(MyAA)` in `lib/Alias/CMakeLists.txt`
5. Add tool in `tools/alias/` if needed

### New Bug Checker

1. Add `include/Checker/MyChecker/` and `lib/Checker/MyChecker/`
2. Extend `llvm::ModulePass` (or a checker base); use `BugReportMgr` for reporting
3. Add to `tools/checker/` and wire into existing tools

### New Pass / Analysis

1. Declare dependencies in `getAnalysisUsage()` (e.g., `AU.addRequired<DyckAliasAnalysis>()`)
2. Use `RegisterPass<T>` for legacy pass registration
3. Add CMake target and link against required `Canary*` libs


## Testing

- Tests live under `tests/unit/` and are grouped by subsystem (`Analysis`, `Checker`, `Concurrency`, `ControlFlow`, `DataFlow`, `Fuzzing`, `IR`, `Pointer`, `Solvers`, `TypeHierarchy`, `Utils`, `Verification`).
- Shared unit-test build helpers are defined in `tests/unit/UnitTestHelpers.cmake`, which is included by `tests/unit/CMakeLists.txt`.
- Add new tests with the subsystem-specific helpers from `tests/unit/UnitTestHelpers.cmake`, e.g. `add_lotus_analysis_test`, `add_lotus_concurrency_test`, `add_lotus_ir_test`, `add_lotus_pointer_test`, `add_lotus_verification_test`.
- Use `add_lotus_targeted_test(...)` only when no existing subsystem helper fits; keep the link set minimal and add subsystem-specific libraries explicitly.
- `add_lotus_pdg_test(...)` is still available for PDG-heavy tests that need the extra LLVM transform utilities.
- Shared test support targets include `lotus_test_utils` and `lotus_test_harness_utils`; prefer them over reintroducing large catch-all link bundles.
- Run all tests with `cd build && ctest --output-on-failure`, or build specific test targets with `cmake --build build --target <test_name>`.
- If a test gains new linker dependencies after CMake cleanup, prefer fixing the relevant subsystem helper in `tests/unit/UnitTestHelpers.cmake` instead of restoring a global "link everything" pattern.
- Clangd warning: ignore all "C++ versions less than C++17 are not supported" warnnings in the test files.


## Documentation

- User guide, architecture, major components: `docs/source/user_guide/`
- Developer guide (adding analyses, checkers, domains): `docs/source/developer/developer_guide.rst`
- Component READMEs: e.g., `lib/Alias/LotusAA/README.md`, `lib/CFL/CSIndex/README.md`
