# Lotus Unit Tests

`tests/unit` contains subsystem-level gtests for Lotus. Its structure follows
the production taxonomy so contributors can move between `include/`, `lib/`,
and `tests/unit/` without translating historical names.

## Layout

Top-level test buckets:

- `Alias/` for alias-analysis and pointer-analysis tests
- `Analysis/` for general analyses, with subdirectories such as `CFG/`,
  `DebugInfo/`, `Loop/`, `NullPointer/`, `Profile/`, and `Purity/`
- `CFL/`
- `Checker/`
- `Concurrency/` with subdirectories such as `MHP/`, `OpenMP/`, `MPI/`,
  `CUDA/`, `LinuxKernel/`, and `Utils/`
- `Dataflow/`
- `Fuzzing/`
- `IR/`
- `Solvers/`
- `SymbolicExecution/`
- `TypeHierarchy/`
- `Utils/`
- `Verification/`
- `TestUtils/` for shared test-only headers

When adding tests, mirror the source tree where practical:

- `include/Alias/...` and `lib/Alias/...` -> `tests/unit/Alias/...`
- `include/Dataflow/...` and `lib/Dataflow/...` -> `tests/unit/Dataflow/...`
- `include/Analysis/CFG/...` and `lib/Analysis/CFG/...` -> `tests/unit/Analysis/CFG/...`

## Build And Run

```bash
cmake -S . -B build
cmake --build build --target <test_target>
ctest --test-dir build --output-on-failure
```

Examples:

- build one target: `cmake --build build --target analysis_misc_tests`
- run one case: `ctest --test-dir build -R CFGUtilitiesTest --output-on-failure`

Every gtest case is discovered individually. Tests also carry a cost/dependency
label:

- `unit` for fast, hermetic tests
- `component` for multi-component or real-concurrency harnesses
- `integration` for tests that invoke external tools or generated fixtures

Run a layer with `ctest --test-dir build -L unit`, `-L component`, or
`-L integration`. New suites default to `unit`; pass `TEST_KIND COMPONENT` or
`TEST_KIND INTEGRATION` to `add_lotus_test_suite` when appropriate.

CTest case granularity does not require one executable per source file. Keep
related test sources in one suite so LLVM and Lotus libraries are linked only
once. A child directory can declare sources with
`lotus_collect_test_sources(<suite> ...)`; the parent then creates the single
binary with `add_lotus_collected_test_suite(<suite> ...)`. GTest discovery
still registers every case independently.

## Adding Tests

Use the small set of helpers in `tests/unit/UnitTestHelpers.cmake`:

- `add_lotus_test_suite` for normal multi-source suites
- `add_lotus_targeted_test` for a single-source test with explicit libraries
- `add_lotus_concurrency_test_suite` for concurrency suites sharing the common
  link set
- `lotus_collect_test_sources` and `add_lotus_collected_test_suite` when child
  directories contribute to one linked binary

Keep test target names stable unless you intentionally want to update external
scripts or CI filters.

`scripts/check_unit_test_registration.py` verifies that every `*.cpp` under
this directory is named by a nearby CMake file. Configuration and CTest both
run the check, preventing silently unbuilt test sources.

## Coverage baseline

Configure a dedicated Clang build with coverage instrumentation, build it, and
generate the unit-layer report:

```bash
cmake -S . -B build-coverage \
  -DLOTUS_BUILD_TESTS=ON \
  -DLOTUS_ENABLE_COVERAGE=ON \
  -DLOTUS_COVERAGE_MINIMUM=0
cmake --build build-coverage
cmake --build build-coverage --target coverage_report
```

The summary is written to `build-coverage/coverage/unit/summary.txt`. Raise
`LOTUS_COVERAGE_MINIMUM` to the accepted total line-coverage percentage once a
baseline has been recorded in CI.
