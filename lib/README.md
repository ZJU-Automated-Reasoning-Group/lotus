# `lib/`

Implementation sources for Lotus libraries.

## Top-level subdirectories

- `Alias/`: Alias analysis
- `Analysis/`: Shared analysis utilities and higher-level analyses.
- `Annotation/`: Infrastructure for attaching, parsing, or propagating analysis
  metadata and source-level annotations.
- `CFL/`: Context-free-language reachability.
- `Checker/`: Various bug-finding engines.
- `Concurrency/`: Analyses for threads, locks, task parallelism, MPI, OpenMP,
  kernels, and related concurrency models.
- `Dataflow/`: Dataflow analysis frameworks and instantiated analyses.
- `Fuzzing/`: Fuzzing-oriented analysis and support components.
- `IR/`: Lotus intermediate representations such as ICFG, GVFG, PDG,
  ShadowMemSSA, SSI, SVFG, and related builders.
- `Optimization/`: Program analysis driven optimization passes.
- `Security/`: Security-focused analyses and verification infrastructure.
- `Solvers/`: SMT and other solver integrations used by analyses,
  verification, and checkers.
- `SymbolicExecution/`: Path-sensitive symbolic execution engine used by the
  `symex` checker frontend.
- `Transform/`: LLVM IR transformation utilities.
- `Utils/`: Shared utilities, support code, data structures, and common LLVM
  helpers.
- `Verification/`: Verification engines.

Keep public headers in `include/` and place their corresponding implementations
here.
