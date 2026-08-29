# Algebraic Program Analysis (the Elimination Method for Dataflow Analysis)

This directory provides **elimination-based** dataflow solvers that compute
solutions by progressively summarizing paths, conceptually similar to state
elimination in automata / Gaussian elimination over path expressions. The
current implementation includes both intraprocedural elimination and a
call-string-based interprocedural layer that reuses the intraprocedural solver
per procedure/context.

## Positioning: what "APA" means here

The directory is named `APA` because it implements an **algebraic, path-expression-based**
style of analysis, including paper-style ADT elimination variants on reducible flowgraphs.

However, this is **not** a full general-purpose implementation of all
"Algebraic Program Analysis" machinery. In Lotus today, this component should
be read as:

- an APA-inspired elimination engine with strong **intraprocedural** support,
- a lightweight **call-string-sensitive interprocedural** extension for selected
  LLVM analyses,
- specialized to LLVM CFG / ICFG clients,
- aimed at MOP-style dataflow clients (reachable, const-prop, RD, liveness,
  etc.).

For broader interprocedural formulations, see other frameworks in this repository
(e.g., IFDS/IDE, WPDS, and NPA modules).

## Public header layout

The public API lives under `include/Dataflow/APA/`:

```text
include/Dataflow/APA/
├── APA.h                          # Canonical umbrella for the framework
├── Core/                          # Generic problem, path-expression, options, results
├── Solver/                        # Solver facades and concrete elimination engines
├── LLVM/                          # LLVM problem and CFG integration
├── Domains/                       # Abstract values and lattice operations
├── Analyses/Intra/                # Intraprocedural LLVM transfer semantics
├── Analyses/Inter/                # Interprocedural LLVM transfer semantics
└── Passes/                        # Legacy-pass wrappers
```

The current layout is the supported public header structure; there are no
compatibility aliases for an older pre-reorg layout.

The public and implementation trees are intentionally not exact mirrors.
`include/Dataflow/APA/` also contains template implementations that must remain
visible to clients, while `lib/Dataflow/APA/` contains only separately compiled
non-template implementations.

### Quick include guide

- Framework umbrella: `#include "Dataflow/APA/APA.h"`
- Minimal intraprocedural surface: `Core/Problem.h`, `Core/Result.h`,
  `Solver/Solver.h`, `LLVM/ForwardProblem.h`
- Minimal interprocedural surface: `Core/InterProblem.h`, `Core/InterResult.h`,
  `Solver/InterSolver.h`
- Abstract domains: `Domains/*.h`
- LLVM clients: `Analyses/Intra/*.h` and `Analyses/Inter/*.h`
- Passes: `#include "Dataflow/APA/Passes/EliminationPasses.h"`
- Internal engine headers: `Solver/SolverContext.h` and the concrete
  `*Solver.h` files are solver internals; downstream clients should normally
  include only `Solver/Solver.h` or `Solver/InterSolver.h`.

## References

### Classical Elimination-Based Dataflow Analysis

NOTE: some of them may not use path expressions.

- Static Analysis by Elimination. Pavle Subotic, Andrew E. Santosa,  and
Bernhard Scholz.
- ETAPS’07:  A new elimination-based data flow analysis framework using
annotated decomposition trees. B. Scholz and J. Blieberger.
- TOPLAS'98: A new framework for elimination-based data flow analysis using DJ graphs. V. C. Sreedhar, G. R. Gao, and Y.-F. Lee. 
- CSUR'86: Elimination Algorithms for Data Flow Analysis. Babara Ryder and Marvin Paull.
- JACM'79: Applications of path compression on balanced trees. R. Tarjan.
- JACM'76: Fast and usually linear algorithm for global flow analysis. S. L. Graham and M. Wegman.
- SIAM J. Comput'77: A simple algorithm for global data flow analysis problems. M. S. Hecht and J. D. Ullman.

### Algebraic Program Analysis (Reps & Kincaid, etc.)

- CAV'21: Algebraic Program Analysis (Tutorial)
- POPL'19: Refinement of Path Expressions for Static Analysis. John Cypher, Jason Breck, Zak Kincaid, Thomas Reps.
- PLDI'17: Compositional Recurrence Analysis Revisited
- FMCAD'15: Compositional Recurrence Analysis 


## What it computes

The solver constructs **path expressions** (regular-expression-like ASTs) over edge transfer functions (`Atom`, `Union`, `Concat`, `Star`) and then evaluates those expressions over your lattice using your `meet` and `applyTransfer`.

This corresponds to a **meet-over-all-paths (MOP)** computation. For classic distributive frameworks, MOP equals the standard maximal fixed point (MFP) solution.

## Relation to `Support/Algorithms/PathExpressions`

Lotus also ships a generic path-expression utility under
`include/Support/Algorithms/PathExpressions/`. That component computes ordinary
regular expressions over edge labels in arbitrary labeled graphs.

The APA solver is different:

- APA path expressions carry **transfer functions**, not plain labels.
- APA expressions are **evaluated over a dataflow lattice** via
  `applyTransfer`, `meet`, and `maxStarIterations`.
- The utility path-expression library is for **regex/path summarization** and is
  not a drop-in implementation of the APA solver.

## Layering

- `Core/` is generic and does not depend on LLVM.
- `Solver/` contains both the generic intraprocedural elimination engines and
  the call-string interprocedural worklist solver.
- `LLVM/` maps LLVM CFGs / ICFGs into the generic problem interfaces.
- `Domains/` owns abstract fact types and their meet/equality operations.
- `Analyses/Intra/` and `Analyses/Inter/` provide LLVM transfer behavior,
  boundary conditions, and solver entry points. A domain may support either or
  both scopes. Shared inter-analysis fact propagation helpers also live under
  `Analyses/Inter/`.

APA is therefore a generic elimination framework, not a complete
"analysis generator." Each client analysis combines a reusable domain with
LLVM-specific modeling.

## Current gaps / non-goals

- **Interprocedural support is deliberately lightweight**: the current solver is
  call-string based, solves one procedure/context at a time, and does not claim
  parity with the repository's IFDS/IDE, WPDS, or NPA frameworks.
- **Not a universal semiring-equation engine**: it uses path-expression elimination with
  problem-defined `meet`/`applyTransfer`, rather than exposing the full range of algebraic
  solver variants used across APA literature.
- **ADT methods are conditional**: `ADTSimple` / `ADTDelayed` require reducible-graph
  assumptions; the solver falls back to `StateElimination` when assumptions do not hold.
- **No claim of complete APA feature parity**: this module does not attempt to cover all
  formulations (e.g., every interprocedural or Newtonian/tensor-product variant).
- **Engineering tradeoff**: path-expression growth can still be substantial on large CFGs;
  this module focuses on practical LLVM analyses rather than full APA
  scalability research coverage.

## Solver methods

Three elimination-style solvers are exposed via `elimination::EliminationOptions`:

- `StateElimination` (default): generic **O(n³)** state-elimination over all nodes (Floyd–Warshall-style).
- `ADTSimple`: **paper-style ADT "simple" algorithm** for **reducible** flowgraphs (O(n²) updates).
- `ADTDelayed`: **paper-style ADT "delayed" algorithm** for **reducible** flowgraphs.

`EliminationOptions` also controls non-convergent `Star` behavior:

- `NonConvergentStarPolicy = Fail | ReturnLast | ReturnIdentity`
- `MaxStarIterations` (0 means use `Problem.maxStarIterations()`).

The public facade `Solver/Solver.h` dispatches to one of three engine headers
in `include/Dataflow/APA/Solver/`:

- `SolverContext.h` (shared internals: reducible-view construction, ADT
  building, expression evaluation)
- `StateEliminationSolver.h`
- `ADTSimpleSolver.h`
- `ADTDelayedSolver.h`

Roughly, the split is:

- `Solver.h`: API surface and fallback policy
- `StateEliminationSolver.h`: generic full-CFG elimination
- `ADTSimpleSolver.h`: eager leaf-update ADT evaluation
- `ADTDelayedSolver.h`: deferred prefix composition with union-find style links

For ADT-based methods, you can optionally implement
`elimination::IntraReducibleEliminationProblem`
(dominators + topological order + edge list). If not provided, the solver computes reducible
flowgraph metadata internally and falls back to `StateElimination` when reducibility assumptions fail.

The synthesized reducible view accepts ADT only when all nodes are entry-reachable,
immediate dominators are computable, and the non-back-edge subgraph is acyclic
with entry first in topological order.

## Summary-equation graph solver

`include/Dataflow/APA/Solver/PathSummaryEquationSolver.h` provides a generic
solver for left-linear path-summary equations:

```text
X_u = base_u U (W_u,v . X_v)
```

Here `X_u` is a summary instance, such as a future `(function, context)` node,
and `W_u,v` is an APA `PathExprFactory` expression. The solver computes SCCs in
the summary-dependency graph, solves SCCs in dependency order, and uses a
state-elimination closure inside cyclic SCCs so
recursive summary dependencies are represented with `Star` expressions rather
than unbounded worklist growth.

This component is intentionally APA-specific: the scheduled objects are
path-expression equations and the output is a closed-form path-expression
summary for each key. It can be used as the algebraic core for a future
interprocedural summary-substitution solver, instead of merely scheduling calls
to an arbitrary intraprocedural analysis.

`PathSummaryEquationSolver` also supports a forward-path mode for equations
where a node summary is extended by outgoing transfer expressions. The forward
interprocedural prototype in
`include/Dataflow/APA/Solver/ForwardInterSummarySolver.h` uses that mode over
instruction/context nodes and labels interprocedural edges with
`InterSummaryTransferAtom` values (`RawNormal`, `CallEntry`, `ReturnExit`, and
`CallToRet`). This gives a real summary-substitution path for forward analyses:
call-entry, return, and bypass effects are represented as path-expression atoms
and recursive context dependencies are closed by SCC-local `Star` expressions.

LLVM client entry points currently include:

- `runInterSummaryElimReachable`
- `runInterSummaryElimConstantPropagation`
- `runInterSummaryElimReachingDefinitions`
- `runInterSummaryElimUninitVariables`
- `runInterSummaryElimLockset`

These are tested for parity with the existing worklist-style interprocedural
solver on focused forward-analysis cases. Lockset wrapper propagation is also
tested directly because the summary graph can preserve a callee-return fact that
the legacy worklist path currently drops. The generic solver is intentionally
forward-only at this stage; backward analyses and affine equalities remain out
of scope for this backend.

## Interprocedural call-string solver

Interprocedural APA clients are modeled by
`elimination::InterEliminationProblem` and solved by
`elimination::InterEliminationSolver<AnalysisDomainTy, K>`. The solver is
context-sensitive via bounded call strings, using
`mono::CallStringCTX<Instruction *, K>` as the context representation.

The solver proceeds by:

- maintaining `IN` / `OUT` facts keyed by `(instruction, call-string context)`,
- computing a boundary fact for one procedure/context from `callFlow` or
  `returnFlow`,
- solving that single procedure with the existing `IntraEliminationSolver`,
- propagating changes across normal, call, return, and call-to-return edges in
  the ICFG worklist.

Clients provide four interprocedural hooks on top of the normal-flow lattice:

- `callFlow(CallSite, Callee, In)` to build the callee-entry fact,
- `returnFlow(CallSite, Callee, ExitStmt, RetSite, In)` to map callee exit facts
  back to the caller,
- `callToRetFlow(CallSite, RetSite, Callees, In)` for the bypass edge,
- `getCalleesOfCallAt(CallSite)` for call resolution.

The default LLVM adapter `LLVMInterEliminationProblem` handles direct calls,
provides a conservative signature-based fallback for indirect calls, and can
optionally warn when indirect-call resolution is missing.

### Context sensitivity

The shipped interprocedural analyses currently use a default call-string bound
of `K = 2`:

- `kDefaultInterElimReachabilityCallStringLength`
- `kDefaultInterElimConstantPropagationCallStringLength`
- `kDefaultInterElimUninitVariablesCallStringLength`
- `kDefaultInterElimReachingDefinitionsCallStringLength`
- `kDefaultInterElimLiveVariablesCallStringLength`
- `kDefaultInterElimLocksetCallStringLength`

`K = 0` is also supported by the generic solver and degenerates to
context-insensitive return propagation.


## Intraprocedural LLVM analyses

We provide a few concrete LLVM IR analyses implemented on top of the
elimination framework. These are intended as practical clients (as in the
paper), and serve as examples for adding additional analyses:

- Reachability (`runIntraElimReachable`)
- Constant propagation (`runIntraElimConstantPropagation`)
- Uninitialized variables (`runIntraElimUninitVariables`)
- Reaching definitions (`runIntraElimReachingDefinitions`)
- Available expressions (`runIntraElimAvailableExpressions`)
- Live variables (`runIntraElimLiveVariables`)
- Lockset analysis (`runIntraElimLockset`)
- Very busy expressions (`runIntraElimVeryBusyExpressions`)
- Non-null propagation (`runIntraElimNonNull`)
- Sign analysis (`runIntraElimSignAnalysis`)

## Interprocedural LLVM analyses

Selected LLVM analyses also expose call-string-sensitive entry points:

- Reachability (`runInterElimReachable`)
- Constant propagation (`runInterElimConstantPropagation`)
- Uninitialized variables (`runInterElimUninitVariables`)
- Reaching definitions (`runInterElimReachingDefinitions`)
- Live variables (`runInterElimLiveVariables`)
- Lockset analysis (`runInterElimLockset`)

These clients reuse the same elimination machinery inside each procedure but
define analysis-specific `callFlow`, `returnFlow`, and `callToRetFlow`
semantics for argument passing, return-value transport, global facts, and
memory effects.

## LLVM pass wrappers

For convenient use under LLVM's legacy pass manager, ten function passes are
provided:

- `-elim-reachable` (reachability)
- `-elim-constprop` (constant propagation)
- `-elim-rd` (reaching definitions)
- `-elim-available` (available expressions)
- `-elim-uninit` (uninitialized variables)
- `-elim-live` (live variables)
- `-elim-lockset` (may-lockset analysis)
- `-elim-busy` (very busy expressions)
- `-elim-nonnull` (nonnull propagation)
- `-elim-sign` (sign analysis)

Use `-elim-method=state|adt-simple|adt-delayed` to select the solver.
Printing is optional via:

- `-elim-reachable-print`
- `-elim-constprop-print`
- `-elim-rd-print`
- `-elim-available-print`
- `-elim-uninit-print`
- `-elim-live-print`
- `-elim-lockset-print`
- `-elim-busy-print`
- `-elim-nonnull-print`
- `-elim-sign-print`

Memory modeling can be toggled with:

- `-elim-use-memssa` (default: true) — use MemorySSA to refine memory analyses

When print flags are enabled, pass output now includes solver diagnostics:
status, requested/executed method, ADT fallback reason, and star-iteration
counters.

## Solver status and result lookup

- `IntraEliminationSolver::solve()` returns `SolveStatus`:
  `Ok`, `FallbackToState`, `NonConvergentStar`, `InvalidProblem`.
- `IntraEliminationSolver::getDiagnostics()` reports method/fallback/counters.
- `DataFlowResultT` uses explicit read lookup:
  `containsNode(node)` and `tryIN(node)` (nullable pointer), and no longer
  returns implicit default facts for missing nodes.
- `InterDataFlowResultT<K, ...>` extends the context-sensitive result type with
  `tryIN(inst, ctx)`, `tryOUT(inst, ctx)`, and `contextsForInstruction(inst)`.

## Analysis coverage notes

Constant propagation now tracks full LLVM `Constant*` values (integers, floats,
vectors, aggregates), uses LLVM constant-folding and instruction-simplification
when operands are constant, and performs alias-aware memory updates when
`AAResults` are available. Uninitialized-variable tracking normalizes pointer
bases, uses ValueTracking for guaranteed-non-undef checks, and clears aliasing
locations via `AAResults` when available, plus basic mem intrinsics.
