# Sparse Value-Flow Graph (SVFG)

This is a re-implementation of the Sparse Value-Flow Graph (SVFG) in SVF. The SVFG provides a sparse representation of value flows, enabling efficient interprocedural analysis with Memory SSA integration.

**This implementation uses [AserPTA](lib/Alias/InclusionBased/AserPTA) as its points-to analysis engine** to compute points-to sets for building value-flow edges and Memory SSA.

## What is SVFG?

SVFG represents value flow in a program as a directed graph where:
- **Nodes** represent definitions and uses (assignments, loads, stores, parameters, Memory SSA defs)
- **Edges** represent direct or guarded value-flow relationships
- **Memory SSA** tracks canonical memory-region versions and connects them to statement nodes

### Example

```c
int x = 5;           // AddrNode (defines x)
int *p = &x;         // CopyNode (p = &x)
int *y = *p;         // LoadNode reads a reaching memory version
*p = 10;             // StoreNode defines the next memory version
```

SVFG representation:
```
AddrNode(x) --> CopyNode(p) --> LoadNode(y)
                                    ^
                                    |
                           IntraIndirect(mem_1)
                                    ^
                                    |
                               StoreNode(*p)
```

## Key Features

- **Sparse value-flow representation** for whole-program analysis
  - Only tracks values that flow through memory or across functions
  - Omits purely local computations (e.g., x = a + b)
  - Reduces graph size while preserving essential def-use information

- **Memory SSA integration** for precise alias tracking
  - Canonical memory regions are keyed by full points-to sets
  - Memory defs are carried by `FormalIn`, `ActualOut`, `IntraMSSAPhi`, and store statements
  - Loads and stores are linked by guarded `IntraIndirect` edges

- **Interprocedural value-flow** through calls and returns
  - FormalIN/FormalOUT: Parameter/return value definitions in callee
  - ActualIN/ActualOUT: Argument/return value uses at call site
  - Call/Return edges connect caller and callee contexts

- **AserPTA-based points-to analysis** (default pointer analysis engine)
  - Guards memory edges with points-to sets (which objects may be accessed)
  - Supports field-sensitive and context-sensitive analysis
  - Enables precise memory dependence tracking

- **On-the-fly call graph refinement** for demand-driven analyses
  - Indirect call edges can be materialized on-demand
  - Supports incremental analysis and query-driven exploration

## DDA-Oriented Design Notes

Lotus DDA (`lib/Alias/DemandDriven/DDA`) consumes SVFG with a few strict assumptions:

- **Object-ID namespace is disjoint from SVFG node IDs**.
  - Object IDs represent abstract memory objects in edge guards.
  - SVFG node IDs represent program-value/memory SSA nodes.
- **Indirect/memory edges carry guard sets of object IDs**.
  - Empty guard means unconstrained flow.
  - Unknown object ID (wildcard) means conservative may-alias-anything.
- **Object metadata is available via SVFG**.
  - `isConstant`, `isUnknown`, `isFunction`, `isHeap`, etc.
  - DDA uses this to prune immutable objects and keep fallback sound.
- **Object IDs are single-namespace and canonical across producers/consumers**.
  - `AddrSVFGNode::getObjectId()` stores the same canonical object ID used in
    guarded indirect edges.
  - `SVFG::getObjectId(value)` and `SVFGBuilder::getObjectIdsForValue(value)`
    must agree on those canonical IDs.
  - Field-object IDs produced by GEPs stay in that same namespace; FI fallback
    is explicit via separate FI object IDs.
- **Indirect callsite indices are maintained**.
  - DDA can discover function-pointer targets and add call/ret edges on demand.
  - Reverse mapping (callee -> invoking indirect callsites) is tracked too.
- **Global memory is rooted at a synthetic module-global entry**.
  - ICFG provides one `GlobalInitBlockNode` per module.
  - SVFG emits `EntryChi` defs at that node and connects them to root-function
    `FormalIn` nodes instead of using `main`/direct-user heuristics.

## Build Pipeline

`SVFGBuilder` executes (conceptually) in these phases:

1. **Pointer analysis bootstrap** (AserPTA) and object-ID mapping.
2. **Node construction**:
   - Top-level statement nodes (`Addr/Copy/Load/Store/Gep/Phi/...`)
   - Inter-procedural nodes (`Actual*/Formal*`)
   - Canonical Memory SSA defs (`FormalIn/ActualOut/IntraMSSAPhi`)
3. **Edge construction**:
   - Direct value-flow edges (copy/gep/phi/param/ret etc.)
   - Guarded indirect memory edges from reaching defs to statement/call sites
4. **Inter-procedural refinement**:
   - Direct-call edges always connected
   - Indirect-call edges optionally deferred for on-the-fly DDA refinement
5. **Memory SSA linking** and optional optimization/update passes.

## Unknown Object Semantics

Unknown object is created lazily and used as a **wildcard** object ID:

- If points-to information is unavailable/ambiguous, edges may carry unknown.
- DDA out-of-budget fallback can use unknown when precise object IDs are absent.
- Unknown preserves soundness but can reduce precision.

## Integration Contract (SVFG <-> DDA)

When modifying SVFG, keep these contracts stable:

- `SVFG::getObjectValue(objId)` and `SVFG::getObjectInfo(objId)` must remain valid.
- `SVFGBuilder::getObjectIdsForValue(value)` must return canonical IDs from the
  same namespace used by `AddrSVFGNode::getObjectId()` and edge guards.
- `SVFG::getIndCallSites`, `getConnectedCallees`, and callsite-ID mappings must
  stay consistent when edges are added on-the-fly.
- Guarded edges should use the same unknown-object convention as fallback paths.

## Usage

### Basic SVFG Construction

```cpp
#include "IR/SVFG/SVFG.h"
#include "IR/SVFG/SVFGBuilder.h"
#include "IR/ICFG/ICFG.h"

// Create ICFG from LLVM module
ICFG *icfg = ICFG::createICFG(module);

// Build SVFG with default configuration
SVFGBuilder builder;
SVFG* svfg = builder.build(icfg);

// Query value flows
SVFGNode* def = svfg->getDef(instruction);
SVFGNodeSet succs = svfg->getSuccs(node);
bool hasPath = svfg->hasPath(src, dst);
```

### Advanced Configuration

```cpp
SVFGBuilderConfig config;
config.buildMSSA = true;                    // Enable Memory SSA
config.resolveIndirectCalls = true;         // Resolve indirect calls via PTA
config.solverType = SVFGBuilderConfig::SolverType::Andersen;
config.memModelType = SVFGBuilderConfig::MemModelType::FieldSensitive;
config.memoryPartition = MemoryRegionPartitionStrategy::InterDisjoint;

SVFGBuilder builder(config);
SVFG* svfg = builder.build(icfg);
```

### Querying SVFG

```cpp
// Get definition node for an instruction
if (SVFGNode *node = svfg->getDef(inst)) {
    // Traverse successors (uses)
    for (SVFGEdge *edge : node->getOutEdges()) {
        SVFGNode *succ = edge->getDstNode();
        // Process successor
    }
}

// Check reachability
if (svfg->hasPath(srcNode, dstNode)) {
    // Value flows from src to dst
}

// Get the guarded memory region attached to a load/store statement
if (auto *load = llvm::dyn_cast<LoadSVFGNode>(node)) {
    const SVFGNodeBS *pts = load->getPointsTo();
    for (uint32_t objId : *pts) {
        const llvm::Value *obj = svfg->getObjectValue(objId);
        // Process pointed-to object
    }
}
```

### Traversing Memory SSA

```cpp
// Find all memory definitions reaching a load
if (auto *load = llvm::dyn_cast<LoadSVFGNode>(node)) {
    for (SVFGEdge *edge : load->getInEdges()) {
        if (edge->getEdgeKind() == SVFGEdgeK::IntraIndirect) {
            SVFGNode *def = edge->getSrcNode();
            // def is a memory definition (store, ActualOut, FormalIn, phi)
        }
    }
}

// Find all uses of a memory definition carried by a store statement
if (auto *store = llvm::dyn_cast<StoreSVFGNode>(node)) {
    for (SVFGEdge *edge : store->getOutEdges()) {
        if (edge->getEdgeKind() == SVFGEdgeK::IntraIndirect) {
            SVFGNode *use = edge->getDstNode();
            // use is a memory user (load/store/call/formal-out)
        }
    }
}
```

## Components

### Core Classes

- **`SVFG`**: Main graph class
  - Manages nodes and edges
  - Provides query interface (getDef, getSuccs, hasPath, etc.)
  - Maintains object metadata for DDA
  - Tracks indirect call sites for on-the-fly refinement

- **`SVFGBuilder`**: Constructs SVFG from ICFG using **AserPTA** for points-to analysis
  - Phases: PTA bootstrap → Node construction → Edge construction → Memory SSA
  - Configurable solver (Andersen, WavePropagation, etc.)
  - Configurable memory model (field-sensitive, field-insensitive)

### Node Types

- **Statement nodes**: Addr, Copy, Load, Store, Gep, BinaryOp, Cmp, Branch
  - Represent direct value definitions from LLVM instructions
  - Example: `x = y` creates a CopySVFGNode

- **PHI nodes**: IntraPhi, InterPhi, IntraMSSAPhi
  - Merge values at control-flow join points
  - IntraPhi: Within a function (loop/branch merge)
  - InterPhi: Across functions (call/return merge)

- **Memory SSA nodes**: FormalIN/OUT, ActualIN/OUT, IntraMSSAPhi
  - Track canonical memory-region versions at function boundaries and CFG joins
  - Enable precise memory dependence analysis

- **Parameter nodes**: FormalParm, ActualParm, FormalRet, ActualRet
  - Connect caller and callee contexts
  - Enable interprocedural value-flow tracking

### Edge Types

- **Intra-procedural edges**: Copy, Load, Store, GEP, Phi, BinaryOp
  - Connect nodes within the same function
  - Represent direct value-flow (def-use chains)

- **Call/Return edges**: CallDir, CallInd, RetDir, RetInd
  - Connect actual arguments to formal parameters
  - Connect return values to call sites
  - Direct: Statically resolved calls
  - Indirect: Function pointer calls (resolved via PTA)

- **Memory edges**: guarded indirect flow and interprocedural memory connectors
  - `IntraIndirect`: Reaching def to load/store/formal-out within a function
  - `CallAIn` / `RetAOut`: Caller/callee memory connectors
  - MHP: May-happen-in-parallel (threading)

## Dependencies

- CanaryICFG, AserPTA, LLVM Core/Support

## References

- Yulei Sui, Ding Ye, Jingling Xue. "Detecting Memory Leaks Statically with Full-Sparse Value-Flow Analysis". TSE'14.
- Yulei Sui, Jingling Xue. "On-Demand Strong Update Analysis via Value-Flow Refinement". FSE'16.
