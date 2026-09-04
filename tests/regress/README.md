# Regression tests

This directory contains file-based, CLI, lit, and historical-bug tests. Tests
are grouped by their owning Lotus module. Inputs and their oracle stay together;
unit versus integration behavior is expressed through CTest labels.

- `Alias/PTA/`: shared pointer-analysis LLVM fixtures.
- `Analysis/TypeHierarchy/`: source fixtures used to generate debug LLVM IR
  for the type-hierarchy integration tests.
- `CFL/Classical/`: Classical CFL CLI and value-flow fixtures.
- `Checker/CLI/`: checker command-line integration tests.
- `Verification/`: CLAM and SymAbsAI regression suites.

Historical fixtures without a registered consumer have been moved out of the
regression tree to `tests/archive/legacy-llvm/`. A regression must have a
registered consumer and a deterministic pass/fail oracle.
