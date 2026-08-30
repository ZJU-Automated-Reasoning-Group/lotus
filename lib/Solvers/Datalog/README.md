# Native C++ Datalog engine

This document describes the current semantic contract and architecture of Lotus's
native Datalog engine. It evolves together with the implementation.
The primary embedding API is strongly typed C++17. Reusable JSON, native Datalog,
and finite Z3 fixedpoint frontends live in `lib/Solvers/Datalog/Frontend` and are
exposed by `Solvers/Datalog/Frontend/Frontend.h`; all lower to the same Semantic
IR. They are not compatibility layers for Ascent's Rust macro syntax.

The frontend implementation is split by language (`Frontend/Json.cpp`,
`Frontend/Lotus.cpp`, and `Frontend/Z3.cpp`), with format dispatch in
`Frontend/Dispatch.cpp`. Every parser produces the data-only, source-aware
`FrontendIR`; `Frontend/Lowering.cpp` validates it and constructs
`SemanticProgram` directly. There is no JSON
serialization/parsing round trip and no frontend class named `Program`.
`SourceUnit` lets embedders compose schema, fact, and rule sources in any
declaration order. Native `.include` directives use an injected `SourceResolver`,
keeping filesystem and path policy outside the engine library.

Public headers and implementation directories mirror each other for `Core`,
`Frontend`, `Runtime`, and `Semantic`. Analyzer and planner implementation files
live under `Core` because they implement `Program::compile()` and expose no
separate public API. The Datalog-wide cross-translation-unit implementation
contract lives in `include/Solvers/Datalog/Internal/Engine.h`; the `Internal`
component marks it as unsupported implementation API.

## Architecture boundary

Every program crosses the following boundary before it executes:

```text
typed C++ API ------------------------------> semantic IR -> analyzer -> RulePlan
JSON / native Datalog / Z3 -> FrontendIR -----^                         -> runtime
```

The template API only constructs typed terms and relations. Rules are immediately
lowered to type-erased `RuleIR`, `AtomIR`, `TermIR`, and `FilterIR` values. Validation
and stratification consume this semantic IR once; execution uses a lowered physical
`RulePlan` with preclassified scan/filter/anti-lookup/aggregate operations, selected
lookup masks, and preprocessed head terms.

## Language semantics

### Relations

A relation is a set of tuples. Inserting an existing tuple has no effect. Column
types and arity are fixed when the relation is created. Values must support
`operator==` and `std::hash`. Equality must be an equivalence relation, equal
values must have equal hashes, and both operations must be pure and non-throwing.
Floating-point NaN values are rejected in relation keys because ordinary C++
floating equality is not reflexive for NaN. Every column of a set relation is a
key column; every column except the final lattice value is a key column in a
lattice relation.

Relations may have zero columns. Such a nullary predicate contains either no tuple
or the single empty tuple and is useful for Boolean query results.

Facts inserted through `Relation::insert` are base facts. Positive SCCs retain their
previous fixed point and propagate only new base and upstream deltas on later runs.
Crossing a negation or non-monotone aggregate dependency invalidates only that
non-monotone suffix. `Relation::erase` removes base facts and conservatively
recomputes dependent SCCs, including facts with alternate proofs.

`CompiledProgram` retains the underlying context state and is therefore safe to run
after the `Context` wrapper is destroyed. Fluent `Relation`, `Var`, and expression
handles remain context-bound construction handles and should not outlive the
wrapper that created them.

### Rules and grounding

The planner may reorder positive atoms, filters, and negations while preserving
aggregate boundaries. A positive body atom grounds every variable it contains.
Repeated occurrences of a variable impose an equality constraint. Constants impose
equality constraints on their columns.

Every variable used by a filter, head term, or head expression must already be
grounded. Invalid programs fail in `program::compile()`; they do not fail during
fixed-point execution.

### Expressions and conditions

Expressions are side-effect-free C++ values evaluated from the current variable
binding. A `where(...)` condition succeeds only when its boolean expression is true.
Expression terms are allowed in rule heads. Body relation terms are variables,
wildcards, or constants; computed body constraints use `where(...)`.

Native `Expr<T>` arithmetic has the semantics of the corresponding C++ operator,
including the caller's responsibility not to trigger undefined signed arithmetic.
The versioned JSON Semantic IR instead uses checked integer arithmetic and rejects
division by zero, remainder by zero, integer overflow, and non-finite floating
inputs or results with a structured evaluation error.

### Recursion

Positive relation dependencies are condensed into strongly connected components.
Acyclic SCCs execute once. Recursive SCCs execute to a least fixed point using
`Total`, `Delta`, and `New` epochs. During an epoch, `Total` is immutable; facts are
deduplicated and merged only at the epoch boundary.

The engine does not guarantee that every valid program terminates. In particular,
a recursive rule may generate an unbounded ascending chain of facts or lattice
values. The v1 contract provides cooperative cancellation but does not yet define
counter-based execution limits.

For a recursive rule, the executor creates one semi-naive variant per recursive
body atom. That occurrence reads `Delta`; earlier recursive occurrences read the
pre-epoch total while later occurrences may read the total including delta. This
leftmost-delta convention makes the variants disjoint instead of relying on final
set deduplication to remove duplicate derivations.

Set-relation deltas hold canonical row IDs rather than copying tuple payloads.
Lattice deltas retain value snapshots so an epoch observes each improvement that
was produced for it.

### Indexes

At an atom, columns containing constants or variables grounded by earlier body
items form a runtime bitmask lookup key. The planner's statistics catalog is
separate from physical indexes. The finalized plan installs reusable ordered-prefix
hash arrangements under configurable count and memory budgets. Arrangements are
maintained incrementally and read without locks during immutable epochs. Lookups borrow binding cells through a zero-allocation key view
and stream rows directly to the consumer; an atom with no bound columns performs a
full scan. Structural `<`, `<=`, `>`, and `>=` filters over ordered native columns
install sorted range access paths instead of scanning the whole relation.

Join costs use cardinality, distinct counts, maximum frequency, and heavy hitters.
Bodies of at most eight atoms use subset dynamic programming; larger bodies use
deterministic beam search. Plans can be rebuilt at a run boundary after large
estimate/cardinality changes. During execution,
variable bindings reference immutable relation cells and own only computed values,
avoiding a `std::any` copy at every join step.

### Aggregation

Aggregation is stratified by default. An aggregate source can be one atom or a
join/filter/anti-join `Body`, avoiding an intermediate relation. Aggregators marked
`monotone()` may participate in recursion when their head is a lattice and their
source contains no negation. `make_streaming_aggregator` exposes a
callback range without materializing all projected inputs. The compatibility
`make_aggregator` API collects a vector. Reducible aggregators expose local state,
merge, and finish operations for parallel execution.

The built-in reducible aggregators are `sum`, `count`, `minimum`, `maximum`, and
`mean`. `make_reducible_aggregator` constructs parameterized custom reducers. They
run serially unless passed `ReducerProperties::parallel()`, which explicitly attests
that `add`/`merge` are associative, commutative, deterministic, and safe to run in
parallel. Generic aggregators may emit zero, one, or multiple results.

`sum<T>`, `minimum<T>`, and `maximum<T>` are parallel-capable only for integral
types (except `bool`). Other types, including floating point and user-defined
types, are serial by default unless an explicitly declared custom reducer attests
to the required laws. `mean<T>` is serial by default because IEEE addition is not
associative. For floating minimum/maximum, NaN is treated as missing whenever a
numeric value exists.

### Lattices

A lattice relation maps a tuple key to one lattice value. Insertion for an existing
key performs `join`; it produces new information only when the stored value changes.
All candidates for a key are joined before that key enters the next delta.

The standard library provides minimum, maximum, set-union, dual, product,
bounded-set, and constant-propagation lattices. User-defined lattice values provide
`bool joinMut(const T &candidate)`; values used through `DualLattice` additionally
provide `meetMut`. Lattice joins must be associative, commutative, and idempotent.
The runtime evaluates joins on copies and commits them only after all parallel
inspection tasks complete, so a throwing join cannot leave live rows or indexes
partially updated.

### Negation

Negation is a stratified anti-join. A negative dependency requires the head stratum
to be strictly greater than the referenced relation's stratum. Every variable in a
negated atom must be grounded before the atom is evaluated.

### Parallel execution

Parallel evaluation uses bulk-synchronous epochs: workers read immutable `Total`
and `Delta`, write thread-local candidates, and merge after a barrier. Ordinary
relations deduplicate candidates; lattice relations locally join by key before the
global join.

The runtime accepts an injected `Scheduler`. `ThreadScheduler` uses a persistent
worker pool and `SerialScheduler` provides deterministic single-thread execution.
Non-recursive rules and recursive delta partitions both execute in parallel when
there is enough work. Only reducers that declare `ReducerProperties::parallel()`
use worker-local states followed by a deterministic merge; generic and undeclared
reducers remain serial. Epoch candidates are hash-partitioned for parallel set
deduplication or per-key lattice joining.

Opaque lifts, reducers, and lattice joins are conservative and serial by default.
`FunctionProperties::parallel()` and `ReducerProperties::parallel()` explicitly
attest purity, determinism, algebraic laws, and thread safety.
`ExecutionOptions::debug_contracts` replays callbacks and samples lattice laws.

One compiled context permits one mutating `run()` at a time. Concurrent mutating
calls are serialized, while `runReadOnly()` calls may share a stable fixed point.
Compilation, relation/variable definition, and relation mutation
during a run fail with `std::logic_error`; load or update base facts between runs.
Read-only relation access should likewise be coordinated by the embedding
application.

`CompiledProgram::run()` returns `RunStatus::Completed` or
`RunStatus::Cancelled`. Cancellation is cooperative and is requested through the
`CancellationToken` in `ExecutionOptions`. If a run is cancelled or an expression,
reducer, or lattice operation throws, all derived state in the run's dirty SCC
closure is discarded while base facts and unaffected stable SCCs remain visible.
Exceptions are rethrown after cleanup. The same compiled program may then be run
again safely.

A compiled plan retains the immutable descriptors it references. Unrelated relation
or variable additions do not invalidate it. Injected schedulers must report at
least one worker.

Native integral and Boolean expression trees are compiled with LLVM ORC JIT.
Checked portable expressions and opaque lifts retain their semantic callbacks.
Simple projection rules use a compiled physical kernel; unsupported rules retain
the interpreter fallback.

## Explicit non-goals

- Rust macro or Rust DSL compatibility
- `ascent_source!` and Rust host-expression parsing
- procedural macros
- WASM support
- distributed execution

## Support matrix

| Feature | Status | Semantics |
| --- | --- | --- |
| Typed relations, variables, constants | implemented | compile-time API checks |
| Conditions and head expressions | implemented | grounded pure expressions |
| Positive recursion and SCCs | implemented | least fixed point |
| Semi-naive execution | implemented | `Total` / `Delta` / `New` epochs |
| Runtime arrangements | implemented | reusable prefixes, budgets, immutable epoch reads |
| Aggregation | implemented | subplans, stratified and declared-monotone recursion |
| Lattice relations | implemented | joined values plus standard lattice library |
| Negation | implemented | stratified anti-join |
| Join planning | implemented | skew statistics, DP/beam search, adaptive replanning |
| Incremental updates | implemented | delta additions; conservative deletion rebuild |
| Goal specialization | implemented | backward pruning and bound magic specialization |
| Typed/JIT kernels | implemented | typed columns, projection kernels, LLVM expressions |
| Parallel execution | implemented | BSP rules, reducers, sharded dedup/index/commit |
| Cooperative cancellation | implemented | dirty derived state is discarded on cancellation |
| Runtime tracing | implemented | SCC, rule, and delta traces |

## CLI and external validation contract

`lotus-datalog` consumes JSON Semantic IR version 1 rather than Rust syntax. Every
program and every result carries `"schema_version": 1`. It emits canonical, sorted
JSON rows plus execution statistics. `lotus-datalog explain` and
`lotus-datalog explain --analyze` expose estimated and actual plans, while
`lotus-datalog-benchmark` provides deterministic storage, skew, proof-explosion,
closure, and incremental workloads. CLI failures use a machine-readable error envelope;
portable arithmetic failures have category `evaluation` and a stable error code.
`lotus-datalog validate` performs parsing, type checking, grounding, dependency
analysis, stratification, SCC construction, and planning without executing the
fixed point.
