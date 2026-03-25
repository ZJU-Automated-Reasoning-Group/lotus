# Monotone Dataflow Analysis

Monotone dataflow analysis frameworks for intraprocedural and interprocedural analysis.

**What gets compiled:** Only the analysis implementations in this directory. All framework code (Core, Solver, Container, Support) is header-only and compiled into consumers.

**Layout (aligned with include/Dataflow/Mono/):**

- **Analyses/Intra/** — Implementation files `Intra*.cpp` (headers `Intra*.h`)
- **Analyses/Inter/** — Implementation files `Inter*.cpp` (headers `Inter*.h`)

**Headers (include/Dataflow/Mono/):** Core interfaces in `Core/Problem.h`, `Core/Domain.h`; solvers in `Solver/IntraSolver.h`, `Solver/InterSolver.h`; results in `Support/Result.h`. See `include/Dataflow/Mono/README.md` for full documentation.
