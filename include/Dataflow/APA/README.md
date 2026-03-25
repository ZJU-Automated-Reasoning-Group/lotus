# APA Dataflow

Header layout for the APA-based intraprocedural elimination framework.

The important architectural split is:

- `Core/`: generic problem, path-expression, options, and result types
- `Engines/`: generic elimination algorithms
- `Adapters/`: CFG/domain adapters, currently LLVM-specific
- `Clients/`: concrete analyses built on top of the framework

There are no compatibility header aliases for the pre-reorg layout. The
directories in this README are the public header structure.

## Directory structure

```
include/Dataflow/APA/
├── APA.h                          # Canonical umbrella for the framework
├── Core/                          # Generic framework API
│   ├── Problem.h                  # IntraEliminationProblem, reducible extension
│   ├── PathExpr.h                 # Path-expression AST
│   ├── Result.h                   # DataFlowResultT
│   └── Options.h                  # Solver options and diagnostics
├── Engines/                       # Canonical solver-engine surface
│   ├── Solver.h
│   ├── SolverContext.h
│   ├── StateEliminationSolver.h
│   ├── ADTSimpleSolver.h
│   └── ADTDelayedSolver.h
├── Adapters/
│   └── LLVM/
│       ├── ForwardProblem.h       # Forward CFG adapter for LLVM instructions
│       └── BackwardProblem.h      # Backward CFG adapter for LLVM instructions
├── Clients/
│   └── LLVM/
│       ├── ExpressionKey.h
│       └── Intra/
│           ├── Reachability.h
│           ├── ConstantPropagation.h
│           ├── ReachingDefinitions.h
│           ├── AvailableExpressions.h
│           ├── UninitializedVariables.h
│           ├── LiveVariables.h
│           ├── VeryBusyExpressions.h
│           └── NonNull.h
└── Passes/                        # LLVM pass wrappers
    └── EliminationPasses.h
```

## Quick include guide

- **Framework umbrella:** `#include "Dataflow/APA/APA.h"`
- **Minimal framework:** `Core/Problem.h`, `Core/Result.h`,
  `Engines/Solver.h`, `Adapters/LLVM/ForwardProblem.h`
- **LLVM clients:** `Clients/LLVM/Intra/*.h`
- **Passes:** `#include "Dataflow/APA/Passes/EliminationPasses.h"` for LLVM pass classes.
- **Internal engine headers:** `Engines/SolverContext.h` and the concrete
  `*Solver.h` files are solver internals; downstream analyses should normally
  include only `Engines/Solver.h`.

## Layering

- `Core/` is generic. It does not know about LLVM.
- `Engines/` is generic. It builds and evaluates path expressions.
- `Adapters/LLVM/` maps LLVM CFGs into the generic problem interface.
- `Clients/LLVM/` defines concrete lattices and transfer semantics for
  particular analyses.

This means APA is a generic elimination framework, but not a complete
"analysis generator." Each client analysis still implements lattice semantics,
transfer behavior, and any LLVM-specific modeling it needs.

## Path-expression terminology

- `Core/PathExpr.h` is APA-specific. It models transfer-function
  expressions (`Atom`, `Union`, `Concat`, `Star`) that the solver later
  evaluates over a dataflow lattice.
- `Utils/Algorithms/PathExpressions/` is a separate utility library for
  Tarjan-style regex construction over generic labeled graphs.
- The two components are related in spirit, but they serve different roles and
  are not interchangeable APIs.

## Solver architecture

- `Engines/Solver.h` owns the shared context and performs method
  dispatch.
- `Engines/SolverContext.h` centralizes shared graph metadata, ADT construction,
  reducibility checks, and expression evaluation.
- `Engines/StateEliminationSolver.h` is the always-available baseline
  algorithm.
- `Engines/ADTSimpleSolver.h` and
  `Engines/ADTDelayedSolver.h` implement the two paper-style
  reducible-graph variants.

Path-expression construction is part of the solver itself, not an incidental
detail. The engines first build path expressions and then evaluate them over a
client-provided lattice. As a result, path-expression growth is one of the main
performance risks in this subsystem.

For ADT methods, the solver accepts either client-provided reducible metadata
(`IntraReducibleEliminationProblem`) or a synthesized reducible view. The
synthesized path is accepted only when all nodes are entry-reachable, immediate
dominators are computable, and removing back edges yields a DAG whose
topological order starts with entry.

Backward clients with multiple exits (for example liveness and very-busy)
typically run one solve per return instruction and combine per-node facts across
solves:

- liveness (may analysis): union
- very-busy (must analysis): intersection

## Runtime status and diagnostics

- `IntraEliminationSolver::solve()` returns `SolveStatus`:
  - `Ok`
  - `FallbackToState`
  - `NonConvergentStar`
  - `InvalidProblem`
- `IntraEliminationSolver::getDiagnostics()` returns `SolveDiagnostics`
  (requested/executed method, ADT usage, fallback reason, star-iteration
  counters).
- Analysis results (`DataFlowResultT`) carry optional solve metadata via:
  - `hasSolveMetadata()`
  - `solveStatus()`
  - `solveDiagnostics()`

## Result lookup semantics

- `DataFlowResultT` no longer returns an implicit default fact for missing
  nodes.
- Read access is explicit:
  - `containsNode(node)`
  - `tryIN(node) -> const FactT *`
- Mutable builders still use `IN(node)`.

## References

See `lib/Dataflow/APA/README.md` for full reference list, including:

- Classical elimination-based dataflow: Aho, Sethi, Ullman (Dragon Book); Muchnik
- Algebraic Program Analysis: Reps & Kincaid papers (2014-2018)
