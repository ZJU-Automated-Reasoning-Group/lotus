# Newtonian Program Analysis

We use the engine in `include/Dataflow/NPA/NPA.h`.

A generic method for solving *interprocedural dataflow equations* by *generalizing Newton’s method** to **ω-continuous semirings*.

The key insight is that Newton’s method can be reformulated **purely algebraically**, without division or limits, and applied to semirings.  
For idempotent semirings this avoids subtraction entirely; non-idempotent domains require a suitable `subtract`.

Analyses are expressed over an **ω-continuous semiring**: $⟨S, +, ·, 0, 1⟩$

- `+`: join / aggregation (may be non-idempotent)
- `·`: sequencing / composition
- supports infinite sums and a natural order `⊑`

This generalizes:
- lattices (classical dataflow analysis),
- language semirings,
- counting semirings,
- probabilistic and cost semirings.

## Solver layers

The current implementation exposes three related layers that are easy to blur
together if you only look at file names:

- **Kleene solver**: solves the full system `X = f(X)` directly by repeated
  evaluation. Public entry point: `KleeneSolver<D>` in `Solver/KleeneSolver.h`.
- **JACM Newton/NPA solver**: solves the same full system `X = f(X)` by outer
  Newton iteration plus a linearized correction solve on each round. Public
  entry point: `NPASolver<D>` in `Solver/NPASolver.h`.
- **TOPLAS tensor-product machinery**: not a separate outer solver. It is an
  optional backend for the inner linearized system `Df|ν(X) + δ = X`, and only
  applies when the current system has LCFL structure and the domain explicitly
  opts into tensor semantics.

That means `LinearStrategy` is only relevant to the **Newton** path. It does
not choose between Kleene and Newton; it only chooses the inner backend used by
Newton for its current linearized system.



## TOPLAS 2016 / LCFL support

The engine supports **TOPLAS 2016**-style algorithms for LCFL (linear
context-free) linear sub-problems that arise inside Newton:

- **LinearStrategy**: `Naive`, `SCC`, `AdaptiveScc`, `TensorProduct`
- **SCC**: Solve in topological order of strongly connected components, using a local dependency-driven worklist within each SCC. This is the ordinary general-purpose inner backend.
- **AdaptiveScc**: Solve the linearized system SCC-by-SCC. Singleton acyclic SCCs use direct evaluation, ordinary recursive SCCs use the SCC worklist solver, and tensor-eligible cyclic LCFL SCCs use the tensor solver locally.
- **TensorProduct**: Rewrite LCFL terms into a tensorized left-linear system, solve there via Tarjan path expressions when extractable to a left-linear graph, and otherwise fall back to tensor-space worklist iteration.
- **TensorDiff**: Direct tensor-side differential builder used by the Newton tensor path.
- **TensorSemiringTraits**: Optional specialization point for domains that want to supply a custom tensor semiring/readout instead of the default exact-correlated tensor domain.
- **LCFLDetector**: `has_lcfl_structure(E1)` detects Concat/Star in linear RHS (used to decide whether tensor is applicable).

`Star` is the paper-faithful Newton/tensor construct. `Mu` is evaluable as a
generic least fixpoint, but NPA rejects it on Newton/tensor paths.

Domains that expose `project()` must additionally opt into `project_newton_safe`
before projection is accepted on Newton/tensor paths.

Use `KleeneSolver<D>::solve(eqns, ...)` for plain Kleene solving.
Use `NPASolver<D>::solve(eqns, verbose, -1, LinearStrategy::SCC)`,
`LinearStrategy::AdaptiveScc`, or `LinearStrategy::TensorProduct` for the JACM
Newton/NPA outer solver with different inner linear backends; or pass
`LinearStrategy` into `BitVectorSolver::run` (optional 5th parameter).

When using `AdaptiveScc`, the solver reports aggregate counts for SCC-local direct/worklist/tensor choices and tensor fallbacks in `Stat`.

## Public header structure

The public API lives under `include/Dataflow/NPA/`:

```text
include/Dataflow/NPA/
├── NPA.h
├── Core/
│   ├── Domain.h
│   ├── DomainExecution.h
│   ├── Symbol.h
│   └── Expr/                  # Immutable equation AST and evaluation
├── Solver/
│   ├── Options.h
│   ├── Statistics.h
│   ├── SolveContext.h
│   ├── Fixpoint.h
│   ├── KleeneSolver.h
│   ├── NPASolver.h
│   └── Newton/
│       ├── Differential.h
│       └── Linear/
│           ├── SccSolver.h
│           ├── AdaptivePlan.h
│           └── Tensor/        # Optional inner Newton backend
├── LLVM/                      # LLVM bit-vector and interprocedural engines
├── Domains/                   # Semiring and transformer domains
├── Analyses/Intra/            # Intraprocedural analysis clients
└── Analyses/Inter/            # Interprocedural analysis clients
```

The public and implementation trees are intentionally not exact mirrors.
`include/Dataflow/NPA/` also contains template implementations that must remain
visible to clients, while `lib/Dataflow/NPA/` contains only separately compiled
non-template implementations.

Notable entry points:

- `Solver/KleeneSolver.h` contains the public Kleene solver.
- `Solver/NPASolver.h` contains the public Newton/NPA solver.
- `Solver/Fixpoint.h` contains low-level fixpoint utilities reused
  internally.
- `Solver/Newton/Differential.h` implements ordinary and tensor-side
  differentials.
- `Solver/Newton/Linear/SccSolver.h` implements the ordinary inner
  linearized-system machinery used by Newton/NPA.
- `Solver/Newton/Linear/Tensor/` contains the optional TOPLAS tensor backend.
- `LLVM/ForwardInterEngine.h` and `LLVM/BackwardInterEngine.h` contain the
  interprocedural LLVM infrastructure.
- `LLVM/BitVectorSolver.h` contains the intraprocedural bit-vector bridge.
- `Analyses/Inter/` contains the public analysis wrappers used by
  the in-tree constant-propagation, interval, taint, nullability, and related
  clients.

## Usage notes

- Intraprocedural clients can use `BitVectorSolver` and related local engines.
- Inter forward clients use `InterEngine<Domain, Analysis>`.
- Inter backward clients use `BackwardInterEngine<Domain, Analysis>`.
- `TransformerSummary` is the current bounded abstract-summary path used
  by in-tree subdistributive clients such as interprocedural constant
  propagation and interval analysis. Transformer carriers live directly under
  `Domains/` because they satisfy the same domain interface as ordinary solver
  domains.

## Current parallel algorithm

The current implementation uses **coarse-grained parallelism inside one
linearized solve**. The important boundary is:

- Newton outer iterations are still sequential
- the current linearized system for a given Newton round may exploit parallelism
- inside that linearized system, the main scheduling unit is the **SCC layer**

This section documents exactly what is parallel today.

### 1. Scope of current parallelism

For one call to `NPASolver<D>::solve(...)`, execution looks like this:

1. construct `nu^(0) = f(bot)`
2. for each Newton round:
   - build the current linearized system `Df|nu(X) + delta = X`
   - solve that linearized system
   - form the next Newton approximant

The current parallel implementation only affects the boxed part below:

`Newton outer loop -> build current linearized system -> solve current linearized system`

It does **not** run multiple Newton rounds concurrently, because round `i+1`
depends on the result of round `i`.

### 2. Parallel Newton setup

For Newton iteration, the solver may parallelize two setup stages for the
**current** round:

- initial value construction `nu^(0) = f(bot)`
- per-round RHS assembly of `(delta + Df|nu)`

This is equation-level parallelism: different equations are prepared on
different workers and then collected in the original equation order.

The parallel setup path is used only when:

- verbose mode is off
- the equation count is large enough
- the thread pool has workers

Expression nodes are immutable. Each evaluation owns an external cache, so
shared AST nodes do not force a serial fallback and do not create cross-solve
data races.

### 3. Parallelism when solving the current linearized system

For `LinearStrategy::SCC`, the linearized system is decomposed into SCCs and the
condensation DAG is solved in topological layers:

- each SCC is solved by a local dependency-driven worklist
- SCCs in the same ready layer may run in parallel
- solved values from a layer are committed only after the whole layer finishes

This makes the current parallelism **layer-parallel**, not fully asynchronous.
Later layers never observe partially committed results from earlier parallel
tasks.

### 4. Adaptive SCC solving

`LinearStrategy::AdaptiveScc` keeps the same layer-parallel scheduling model,
but chooses the local solver independently for each SCC:

- `Direct` for singleton acyclic SCCs
- `Worklist` for ordinary recursive SCCs
- `Tensor` for tensor-eligible cyclic LCFL SCCs

Parallelism is still only across independent SCCs in the same layer. Each
individual SCC solve remains serial in v1, including tensor-eligible SCCs.

So `AdaptiveScc` changes **which** solver is used per SCC, but does not yet
change the granularity of parallel scheduling.

### 5. What is intentionally serial today

The following are not parallelized today:

- outer Newton rounds
- worklist iteration inside one cyclic SCC
- tensor-path solving inside one SCC beyond the existing tensor algorithm itself
- top-level orchestration of multiple independent NPA solves
- interprocedural graph construction and propagation as a whole-program task

In other words, the current design treats:

- the **equation** as the unit of parallel setup work
- the **SCC** as the unit of local solver choice
- the **SCC layer** as the unit of parallel scheduling

### 6. Practical consequence

The current implementation benefits most when the linearized system has:

- enough equations to make Newton setup parallelism worthwhile
- multiple independent SCCs in the same condensation layer
- mixed SCC structure, so `AdaptiveScc` can avoid using one global inner solver

If a benchmark is dominated by one large cyclic SCC, the current implementation
will still solve that SCC serially. In that case, the main benefit of
`AdaptiveScc` is solver selection, not parallel speedup.

## Discussion and further directions

### 1. More outer-level parallelization

There are two different "outer-level" ideas:

- **parallelizing different Newton rounds of the same solve**
- **parallelizing multiple independent top-level NPA solves**

The first is difficult and mostly against the structure of Newton iteration:

- round `i+1` needs the concrete result of round `i`
- the next differential `Df|nu^(i+1)` cannot be built before `nu^(i+1)` exists

So outer Newton rounds form a true dependence chain. Any attempt to parallelize
them would be speculative and would change the algorithmic design substantially.

The second is much more realistic:

- different analyses on the same module may be run concurrently at the driver level
- different modules or different client queries may be run concurrently
- this is conceptually cleaner because the solves are independent

This kind of "more outer-level" parallelization is not currently implemented in
the NPA core itself, but it is a plausible systems direction.

### 2. Parallelization inside each SCC

Parallelization inside one cyclic SCC is the most natural next fine-grained
direction, but it is also the hardest to design cleanly.

Why it is hard:

- variables inside one SCC are mutually recursive
- workers would either share mutable state or work from stale snapshots
- deterministic accounting for `max_linear_steps` becomes less obvious
- naive parallel worklists often suffer from synchronization overhead or poor load balance

A likely principled design would be bulk-synchronous:

- take a snapshot of the current SCC environment
- evaluate a partition of equations in parallel
- merge updates deterministically
- repeat until stable

That is much cleaner than fully asynchronous chaotic updates, but it is a new
algorithmic design, not just an engineering extension of the current code.

### 3. Why SCC granularity remains a good current boundary

Even if parallelism is limited, SCC granularity is still useful because it gives:

- a clean structural decomposition of the current linearized system
- a natural place to select `Direct`, `Worklist`, or `Tensor`
- deterministic layer-parallel scheduling where it exists
- useful diagnostics about where tensorization helps or falls back

So the present implementation should be viewed as:

- a **structure-adaptive SCC-local solver**
- with **coarse-grained parallelism across SCC layers**

rather than as a fully parallel Newton solver.

## Related Work

- Compositional Recurrence Analysis Revisited. PLDI 17.
- Newtonian Program Analysis via Tensor Product. POPL 16.
- Newtonian Program Analysis, JACM 10.
