# Regression tests

This directory contains file-based, CLI, lit, and historical-bug tests. Tests
are grouped by their owning Lotus module. Inputs and their oracle stay together;
unit versus integration behavior is expressed through CTest labels.

- `Alias/PTA/`: shared pointer-analysis LLVM fixtures.
- `CFL/Classical/`: Classical CFL CLI and value-flow fixtures.
- `Checker/CLI/`: checker command-line integration tests.
- `Verification/`: CLAM and SymAbsAI regression suites.
- `Legacy/LLVM/`: historical generated fixtures awaiting consumer/owner audit.

New tests should not be added to `Legacy/`. A regression must have a registered
consumer and a deterministic pass/fail oracle.
