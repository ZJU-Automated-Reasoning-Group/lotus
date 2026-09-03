GVFG
====

The Guarded Value-Flow Graph (GVFG) is a per-function IR for value flow,
memory flow, and path-sensitive dependencies. It extends plain value-flow
reasoning with explicit guard objects, block conditions, memory producers, and
call-boundary interface nodes so clients can ask not just whether a value may
flow, but under which conditions that flow is feasible.

**Headers**: ``include/IR/GVFG/``

**Implementation**: ``lib/IR/GVFG/``

**Library target**: ``CanaryGuardedValueFlow``

Overview
--------

GVFG is designed for analyses that need more precision than raw SSA def-use or
an unguarded value-flow graph, especially around loads, stores, conditionals,
PHIs, and calls. The graph is owned by ``GuardedValueFlowGraph`` and is built
in two layers:

1. ``GuardedValueFlowGraphBuilderPass`` constructs the structural,
   intra-procedural graph directly from LLVM IR.
2. ``LotusGuardedValueFlowAdapterPass`` optionally enriches that graph with
   LotusAA-backed memory producers, imported path conditions, summary channels,
   and call-boundary metadata.

The resulting graph keeps value dependencies and control/path constraints in one
representation, without forcing every client to rebuild feasibility logic on top
of a lower-level IR.

Core graph model
----------------

``GuardedValueFlowGraph`` stores one base function and owns:

- Value nodes for SSA operands, arguments, returns, PHIs, opcode results, loads,
  stores, casts, and unknown/imported placeholders.
- Region nodes for block-level and composed boolean conditions.
- Sites for semantically important instructions such as calls, returns,
  dereferences, GEPs, compares, divisions, and allocations.
- Auxiliary mappings from LLVM ``Value*`` and ``Instruction*`` objects to graph
  nodes, regions, call sites, return sites, guard nodes, and memory nodes.

Edges are directed from a result or consumer to the node it depends on. Each
edge can carry:

- A ``confidence`` score for conservative or imported information.
- A ``ConditionRef`` describing the guard under which that dependency holds.

The graph also records diagnostics via ``GuardedValueFlowGraph::Diagnostic`` so
clients can detect degraded precision introduced by the builder or adapter.

Node categories
---------------

``GuardedValueFlowNode::Kind`` covers several families of nodes:

- Argument and return nodes: ``CommonArgument``, ``PseudoArgument``,
  ``VariableArgument``, ``CommonReturn``, and ``PseudoReturn``.
- Plain value and memory nodes: ``SimpleOperand``, ``UndefValue``,
  ``LoadMemory``, and ``StoreMemory``.
- Structural nodes: ``Phi`` and ``Region``.
- Call-boundary nodes: ``CallSiteCommonOutput``, ``CallSitePseudoInput``,
  ``CallSitePseudoOutput``, ``CallSiteArgumentSummary``, and
  ``CallSiteReturnSummary``.
- Condition and computation nodes: ``InterfaceCondition``, ``SimpleOpcode``,
  ``CastOpcode``, and ``Unknown``.

Two parts of the model are especially important:

- ``LoadMemory`` and ``StoreMemory`` nodes separate memory-producing facts from
  ordinary SSA value flow, which lets clients ask which stores or summaries may
  define a load.
- Interface and summary nodes let interprocedural clients reason about call
  boundaries without collapsing everything into raw call/return edges.

Guard and region modeling
-------------------------

Path feasibility is represented explicitly with ``GuardedValueFlowRegionNode``
and ``ConditionRef``.

``ConditionRef`` distinguishes:

- ``None`` for unconditional flow.
- ``StructuralGuard`` for branch- or switch-based conditions.
- ``SemanticPathCond`` for imported semantic path conditions.

``GuardedValueFlowRegionNode::Form`` supports:

- ``AlwaysTrue`` and ``AlwaysFalse`` sentinels.
- ``Unit`` regions tied to a single condition node and branch sense.
- ``Semantic`` and ``ImportedInterface`` regions for symbolic path conditions.
- ``And``, ``Or``, and ``Not`` composition for guard refinement.

At the block level, ``GuardedValueFlowGraph::BlockCondition`` stores the guard
producer, controlling block, successor, condition reference, and branch sense.
Non-region nodes inherit a parent-block region by default, and the adapter can
later rewrite or refine those placements when imported facts are available.

Sites and instruction events
----------------------------

GVFG keeps instruction-level events in dedicated site objects so the node graph
does not need to encode every semantic detail directly.

``GuardedValueFlowSite::Kind`` includes:

- ``CallSite``
- ``ReturnSite``
- ``DereferenceSite``
- ``GEP``
- ``Compare``
- ``Div``
- ``Alloc``

Important site types include:

- ``GuardedValueFlowCallSite`` for common inputs, common output, per-callee
  pseudo inputs and outputs, summary nodes, back-edge metadata, and
  callee-specific conditions.
- ``GuardedValueFlowDereferenceSite`` for the pointer operand of a load or the
  pointer/value operands of a store.
- ``GuardedValueFlowGEPReferenceSite`` for base pointers, offset operands, and
  the final result node.
- ``GuardedValueFlowCompareSite`` and ``GuardedValueFlowDivSite`` for arithmetic
  and comparison-sensitive reasoning.

Builder and adapter pipeline
----------------------------

The construction pipeline is intentionally split:

- ``GuardedValueFlowGraphBuilderPass`` builds the structural graph for each
  function and stores it in a per-module pass-managed cache.
- ``GuardedValueFlowBuilder.h`` is a compatibility shim that aliases the newer
  builder pass name for older includes.
- ``LotusGuardedValueFlowAdapterPass`` replaces placeholder memory edges with
  LotusAA-backed producers and materializes richer interprocedural interface and
  summary information.
- ``LotusGuardedValueFlowAdapterPass::safeLink`` attaches dependencies while
  preserving confidence and guard metadata.

When LotusAA's must-kill optimization is enabled, the adapter receives only
the surviving roots of the incremental kill forest for each load. Conditional
roots retain their ``path_cond_t`` provenance, so GVFG matching regions still
represent the fallback blocking conditions for stores that cannot be killed
statically.

This split keeps the core graph useful on its own while allowing memory and
interprocedural precision to be layered in when LotusAA is available.

Query and solver support
------------------------

``GuardedValueFlowGraph`` exposes direct graph-level queries such as:

- ``getDirectDataDependencies(...)``
- ``getEffectiveControlDependencies(...)``
- ``getMemoryProducers(...)``
- ``getResolvedCallTargets(...)``

For symbolic reasoning, ``GuardedValueFlowSolver`` translates guarded
dependencies into SMT constraints. Its API supports:

- Control dependencies for a node or block.
- Data dependencies for a node.
- Guarded PHI incoming constraints.
- Combined dependency queries for one assignment edge.
- Encoding of opcode nodes such as arithmetic, casts, compares, GEPs, select,
  and vector element operations.

``DTGuardedValueFlowSolver`` adds dominator awareness so previously established
control constraints can be suppressed when they are already implied by the
current traversal context.

Serialization and debugging
---------------------------

``GuardedValueFlowSerializer`` is the main inspection utility for GVFG. It can:

- Emit text output through ``toText(...)`` and ``writeText(...)``.
- Emit DOT graphs through ``toDot(...)`` and ``writeDot(...)``.

The serializer includes node kinds, descriptions, regions, LLVM value links,
access paths, edges, and recorded sites, which makes it useful when debugging
builder or adapter behavior.

Main headers
------------

- ``GuardedValueFlowGraph.h`` defines the graph container, block conditions,
  diagnostics, query helpers, and the builder pass.
- ``GuardedValueFlowNodes.h`` defines the node hierarchy, access paths, PHI
  incoming metadata, call output nodes, summary nodes, and region forms.
- ``GuardedValueFlowSites.h`` defines instruction-level event records for calls,
  dereferences, returns, GEPs, compares, divisions, and allocations.
- ``ConditionRef.h`` defines the lightweight handle used to name structural or
  semantic guard conditions.
- ``GuardedValueFlowSolver.h`` defines the SMT-backed feasibility solver and its
  dominator-aware variant.
- ``GuardedValueFlowSerializer.h`` defines text and DOT export helpers.
- ``LotusAdapter.h`` defines the LotusAA integration pass.

Use cases
---------

- Guard-aware dataflow and dependence reasoning.
- Explaining whether a value-flow edge is unconditional, branch-guarded, or
  imported from a semantic path fact.
- Tracking memory producers for loads without collapsing memory flow into plain
  SSA edges.
- Modeling call-boundary summaries and pseudo side-effect channels.
- Exporting richer per-function flow graphs for experiments, debugging, and
  downstream solver-based analyses.

See also
--------

- See :doc:`svfg` for the unguarded sparse value-flow graph.
- See :doc:`pdg` for dependence-based querying.
