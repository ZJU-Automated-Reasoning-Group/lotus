# Guarded Value-Flow Graph (GVFG)

GVFG is a per-function IR for value-flow, memory-flow, and path-sensitive
dependencies. It is built in two stages:

1. The structural builder creates nodes and sites from LLVM IR.
2. The optional LotusAA adapter enriches the graph with alias- and
   memory-derived flow, summaries, and imported path facts.

When LotusAA's must-kill optimization is enabled, the adapter receives only
the surviving roots of the incremental kill forest for each load. Conditional
roots retain their `path_cond_t` provenance, so GVFG matching regions still
represent the fallback blocking conditions for stores that cannot be killed
statically.

GVFG is intended for clients that need more than plain SSA def-use edges,
especially when loads, stores, calls, and control guards must be modeled
explicitly.

## Core Model

`GuardedValueFlowGraph` owns the graph for one base function. The graph
contains:

- **Value nodes** for SSA values and helper values such as arguments, returns,
  PHIs, opcode nodes, casts, loads, stores, and unknown/imported values.
- **Region nodes** for path conditions and boolean guard composition.
- **Call-boundary nodes** for common outputs, pseudo inputs/outputs, and
  summary channels.
- **Sites** for semantically important instructions such as calls, returns,
  dereferences, GEPs, compares, divisions, and allocations.

Edges are directed from a result or consumer to the value, memory node, or
helper node it depends on. Each edge may carry:

- a `confidence`
- a guarded `ConditionRef`

This lets clients distinguish unconditional dependencies from dependencies that
only hold on a specific path or imported summary condition.

## Node Categories

The main node kinds exposed by `GuardedValueFlowNode::Kind` are:

- `CommonArgument`, `PseudoArgument`, `VariableArgument`
- `CommonReturn`, `PseudoReturn`
- `SimpleOperand`, `UndefValue`
- `LoadMemory`, `StoreMemory`
- `Phi`
- `Region`
- `CallSiteCommonOutput`, `CallSitePseudoOutput`, `CallSitePseudoInput`
- `CallSiteArgumentSummary`, `CallSiteReturnSummary`
- `InterfaceCondition`
- `SimpleOpcode`, `CastOpcode`
- `Unknown`

Two details matter in practice:

- **Memory nodes** separate value flow from memory producer flow, so a load can
  ask which stores or summary producers may define it.
- **Interface/summary nodes** model interprocedural flow without forcing every
  client to reason directly about raw call edges.

## Region And Guard Modeling

`GuardedValueFlowRegionNode` represents the control/path-sensitive part of the
graph. Region forms include:

- `AlwaysTrue`, `AlwaysFalse`
- `Unit`
- `Semantic`
- `ImportedInterface`
- `And`, `Or`, `Not`

The graph stores block-level guarding facts in `BlockCondition` records. A
condition links:

- the guard-producing node
- the controlling basic block
- the successor that makes the condition true
- the `ConditionRef`
- the branch sense

Non-region nodes inherit the region of their parent block unless later
rewritten by the adapter.

## Sites

Sites preserve instruction-level events that are useful to downstream analyses
without overloading the node graph itself.

`GuardedValueFlowSite::Kind` includes:

- `CallSite`
- `ReturnSite`
- `DereferenceSite`
- `GEP`
- `Compare`
- `Div`
- `Alloc`

For example:

- `GuardedValueFlowCallSite` tracks common inputs, common output, per-callee
  pseudo inputs/outputs, summary nodes, back-edge callees, and callee guards.
- `GuardedValueFlowDereferenceSite` records pointer/value operands for loads
  and stores.
- `GuardedValueFlowGEPReferenceSite` records the base pointer, offset operands,
  and result node.

## Main APIs

Key entry points:

- `GuardedValueFlowGraph`
- `GuardedValueFlowGraphBuilderPass`
- `GuardedValueFlowSerializer`

Useful query APIs on `GuardedValueFlowGraph`:

- `getDirectDataDependencies(...)`
- `getEffectiveControlDependencies(...)`
- `getMemoryProducers(...)`
- `getResolvedCallTargets(...)`

The graph also exposes lookup/mapping helpers for:

- LLVM `Value*` to node mappings
- call and return sites
- synthetic guard nodes
- load/store memory nodes
- pseudo arguments/returns
- function summary interfaces

## Diagnostics And Degraded Precision

`GuardedValueFlowGraph` stores diagnostics emitted by the builder or adapter.
Each diagnostic records:

- origin: `Builder` or `Adapter`
- severity: `Note`, `Warning`, or `Error`
- message plus optional instruction/block context

Clients can use `diagnostics()`, `hasDiagnostics()`, and `isDegraded()` to
detect when the graph fell back to a conservative representation.

## Debugging Support

`GuardedValueFlowSerializer` can export GVFG as:

- text (`GVFG-TEXT-V1`)
- DOT

The serializer includes node kind, description, type, block, LLVM value,
access path, edges, and recorded sites, which makes it the main inspection
tool when debugging builder or adapter behavior.
