# Monotone Dataflow Analysis

Monotone dataflow analysis frameworks for intraprocedural and interprocedural analysis.

**What gets compiled:** Only the analysis implementations in this directory.
Framework code under `include/Dataflow/Mono/` is template-based or header-only
and compiled into consumers.

**Public layout:**

- **Core/** — generic call-string context representation
- **Solver/** — intra/inter solvers and the call-string engine
- **LLVM/** — LLVM problem interfaces and solver-facing analysis types
- **Domains/** — named abstract fact domains used by concrete analyses
- **Container/** — `BitVectorSet.h`, `Traits.h`
- **Support/** — `Result.h`, `MonoDebug.h`, `Soundness.h`
- **Analyses/Intra/** — intraprocedural analysis clients
- **Analyses/Inter/** — interprocedural analysis clients

The public and implementation trees are intentionally not exact mirrors.
`include/Dataflow/Mono/` contains template implementations that must remain
visible to clients, while `lib/Dataflow/Mono/` contains only separately
compiled concrete analyses.

## Quick include and migration guide

- LLVM problem API: `#include "Dataflow/Mono/LLVM/Problem.h"`
- LLVM analysis types: `#include "Dataflow/Mono/LLVM/AnalysisTypes.h"`
- Intraprocedural solver: `#include "Dataflow/Mono/Solver/IntraSolver.h"`
- Interprocedural solver: `#include "Dataflow/Mono/Solver/InterSolver.h"`
- Call-string engine: `#include "Dataflow/Mono/Solver/CallStringSolver.h"`
- Concrete domains: `#include "Dataflow/Mono/Domains/*.h"`

## Architecture

- `IntraMonoProblem` models intraprocedural analyses with `normalFlow()`,
  `merge()`, `equal_to()`, and `initialSeeds()`.
- `InterMonoProblem` extends that interface with `callFlow()`,
  `returnFlow()`, and `callToRetFlow()`.
- `CallStringContext` and `CallStringSolver` provide bounded call-string
  context sensitivity for interprocedural clients.
- `Container/BitVectorSet.h` provides a bit-vector-backed set implementation
  for larger finite universes.

## API notes

- `CallStringInterProceduralDataFlowEngine::applyForwardFromSeeds()` returns
  `std::unique_ptr<ResultTy>`.
- Callee and return-site resolution in the call-string engine is driven by the
  provided ICFG (`getCalleesOfCallAt`, `getReturnSitesOfCallAt`, etc.), not a
  separate callback parameter.
