# Demand-Driven Pointer Analysis (DDA)

Demand-driven pointer analysis computes points-to sets on-demand by backward traversal through the Sparse Value-Flow Graph (SVFG), following **SVF's FlowDDA / DDAVFSolver** design (FSE'16, TSE'18).

Unlike exhaustive pointer analysis that computes points-to sets for all pointers upfront, DDA only analyzes pointers when queried, making it efficient for query-driven tools and large codebases.

## What is Demand-Driven Analysis?

**Traditional (Exhaustive) Pointer Analysis:**
```
Input: Entire program
Process: Analyze ALL pointers
Output: Points-to sets for every pointer
Cost: High (analyzes everything)
```

**Demand-Driven Analysis:**
```
Input: Specific pointer query
Process: Backward traverse SVFG from query point
Output: Points-to set for queried pointer only
Cost: Low (analyzes only what's needed)
```

### Example

```c
int x, y, z;
int *p = &x;        // p -> {x}
int *q = &y;        // q -> {y}
int *r = &z;        // r -> {z}
if (cond)
  p = &y;           // p -> {x, y}
int a = *p;         // Query: what does p point to here?
```

**Exhaustive analysis:** Computes points-to for p, q, r (even though we only query p)
**Demand-driven:** Only computes points-to for p by backtracking from the query point

## Scope In Lotus

Lotus provides two DDA modes and multiple client types:

### Analysis Modes

- **`FlowDDA`**: Flow-sensitive, context-insensitive
  - Distinguishes different program points (flow-sensitive)
  - Merges all calling contexts (context-insensitive)
  - Faster, suitable for most use cases
  - Example: Distinguishes `p` before and after assignment

- **`ContextDDA`**: Flow-sensitive, context-sensitive
  - Distinguishes different program points AND calling contexts
  - More precise for recursive functions and callbacks
  - Slower but handles complex call patterns better
  - Example: Distinguishes `p` in different call chains

### Client Types

- **`DDAClient`** (All): Analyzes all top-level pointers
  - Use for: Comprehensive whole-program analysis
  - Collects: All pointer-typed values in the program

- **`FunptrDDAClient`** (Funptr): Analyzes function pointers at indirect call sites
  - Use for: Call graph construction, virtual call resolution
  - Collects: Function pointers used in indirect calls
  - Example: Resolving callbacks, virtual methods

- **`AliasDDAClient`** (Alias): Analyzes pointers in memory operations
  - Use for: Alias-driven optimizations
  - Collects: Load sources, store destinations, GEP bases
  - Example: Redundant load elimination

### Supporting Components

- **`DDAVFSolver`**: Generic backward solver (CRTP base class)
- **`DDAPass`**: Driver that selects solver mode and client
- **`DPItem`**: Demand-driven program item (query state)

## Core Query Algorithm

Given a queried pointer value `p`, DDA computes its points-to set through backward traversal:

### Algorithm Steps

1. **Initialize**: Map `p` to its defining SVFG node and create DPM state `(cur, loc)`
   - `cur`: Current pointer/object node ID
   - `loc`: Current SVFG location (program point)

2. **Backward Traversal**: Run `findPT(dpm)` in `DDAVFSolver`
   - Dispatch by SVFG node kind:
     - **Addr**: Add allocation object to points-to set
     - **Copy/Phi**: Continue backward through operands
     - **Gep**: Adjust field offsets in points-to set
     - **Load**: Get points-to of pointer, then indirect backward traversal
     - **Store**: Apply strong/weak update based on must-alias
     - **Memory nodes**: Handle MU/CHI for memory SSA
   - Traverse direct/indirect incoming value-flow edges
   - Recurse on predecessor DPMs; union returned points-to sets

3. **Caching**: Store computed points-to sets to avoid recomputation
   - Top-level pointer cache: `dpmToTLPtsMap_`
   - Address-taken/memory cache: `dpmToADPtsMap_`

4. **Refinement**: When new facts appear for a DPM, trigger `reCompute` on dependents
   - Previously visited states can be refined with new information
   - Ensures fixpoint convergence

5. **Termination**: Stop when query reaches fixpoint or hits budget
   - Fixpoint: No new points-to facts discovered
   - Budget: Maximum traversal steps exceeded

### Example Walkthrough

```c
int x, y;
int *p = &x;        // L1: p -> {x}
if (cond)
  p = &y;           // L2: p -> {y}
int z = *p;         // L3: Query p
```

Backward traversal from L3:
```
1. Start: DPM(p, L3)
2. Find phi node merging p from both branches
3. Backward to L2: DPM(p, L2) -> find "p = &y" -> add y to pts
4. Backward to L1: DPM(p, L1) -> find "p = &x" -> add x to pts
5. Result: p -> {x, y}
```

## Usage Examples

### Basic Usage (FlowDDA)

```cpp
#include "Alias/DDA/FlowDDA.h"

// Initialize DDA
FlowDDA dda;
dda.run(module);

// Query points-to set
auto pts = dda.getPointsTo(ptr);
for (uint32_t objId : pts) {
    const llvm::Value *obj = dda.getSVFG()->getObjectValue(objId);
    // Process pointed-to object
}

// Check alias
if (dda.mayAlias(ptr1, ptr2)) {
    // ptr1 and ptr2 may alias
}

// Check null
if (dda.mayNull(ptr)) {
    // ptr may be null
}
```

### Using DDAPass with Client

```cpp
#include "Alias/DDA/DDAPass.h"

// Create DDA pass
DDAPass dda;

// Select analysis mode
dda.setDDAKind(DDAKind::FlowS_DDA);  // or DDAKind::Cxt_DDA

// Select client type
dda.selectClient(DDAClientKind::Funptr);  // Analyze function pointers

// Run analysis
dda.runOnModule(module);

// Query results
if (dda.mayAlias(ptr1, ptr2)) {
    // ptr1 and ptr2 may alias
}
```

### Custom Query Set

```cpp
#include "Alias/DDA/DDAPass.h"

DDAPass dda;

// Add specific pointers to query
dda.addQuery(ptr1);
dda.addQuery(ptr2);
dda.addQuery(ptr3);

// Run analysis (only analyzes added queries)
dda.runOnModule(module);
```

### Context-Sensitive Analysis

```cpp
#include "Alias/DDA/ContextDDA.h"

// Use context-sensitive mode for better precision
DDAPass dda;
dda.setDDAKind(DDAKind::Cxt_DDA);
dda.runOnModule(module);

// More precise results for recursive/callback-heavy code
auto pts = dda.getFlowDDA()->getPointsTo(ptr);
```

### Budget Control

```cpp
#include "Alias/DDA/FlowDDA.h"

// Set maximum traversal steps per query
FlowDDA::setDefaultMaxBudget(10000);  // Default: 100000

FlowDDA dda;
dda.run(module);

// Queries exceeding budget fall back to conservative PTA
auto pts = dda.getPointsTo(ptr);
```

### Custom Client

```cpp
#include "Alias/DDA/DDAClient.h"
#include "Alias/DDA/FlowDDA.h"

class MyClient : public DDAClient {
public:
    std::vector<const llvm::Value *> &collectCandidateQueries() override {
        // Collect custom set of pointers
        for (auto &F : *getModule()) {
            for (auto &BB : F) {
                for (auto &I : BB) {
                    if (/* custom condition */) {
                        addCandidate(&I);
                    }
                }
            }
        }
        return candidateQueries_;
    }

    void handleStatement(const SVFGNode *node, uint32_t curNodeId) override {
        // Hook into backward traversal
        // Called for each visited SVFG node
    }

    void performStat(FlowDDA *dda) override {
        // Collect custom statistics
    }
};

// Use custom client
MyClient client;
FlowDDA dda;
dda.setClient(&client);
dda.run(module);
dda.answerQueries();
```

## Out-Of-Budget Fallback

Lotus follows SVF's design of conservative fallback when a demand query exceeds
the step budget:

- Primary fallback source: pointer-analysis object sets from `SVFGBuilder`
  (`AserPTA`-backed in Lotus).
- Candidate fallback values are collected from:
  - query-location value (`dpm.getLoc()->getValue()`), and
  - object-mapped value (`svfg->getObjectValue(dpm.getCurNodeID())`).
- If no object ID can be recovered, fallback inserts the SVFG unknown object.

This guarantees a non-empty conservative result whenever unknown object exists,
while still preferring precise PTA-derived IDs.

## Context Sensitivity

`ContextDDA` extends the same transfer logic with call-string constraints:

- Call/return edges update or match context IDs
- Recursive callsites are treated context-insensitively where needed
- Context-insensitive edge set is initialized from recursion/value-flow cycles
- Out-of-budget in `ContextDDA` downgrades to conservative object-level fallback

### When to Use Context-Sensitive Analysis

**Use ContextDDA when:**
- Code has deep recursion or complex callback patterns
- Precision is critical (e.g., security analysis)
- Willing to trade performance for accuracy

**Use FlowDDA when:**
- Code is mostly non-recursive
- Performance is critical
- Moderate precision is acceptable

### Comparison

| Aspect | FlowDDA | ContextDDA |
|--------|---------|------------|
| Flow-sensitivity | ✓ | ✓ |
| Context-sensitivity | ✗ | ✓ |
| Precision | Moderate | High |
| Performance | Fast | Slower |
| Memory usage | Low | Higher |
| Best for | General use | Recursive/callback code |

## Performance Characteristics

### Scalability

- **Query time**: O(k) where k = number of SVFG nodes visited
- **Memory**: O(n) where n = number of cached DPMs
- **Budget control**: Limits worst-case query time
- **Caching**: Amortizes cost across multiple queries

### Optimization Tips

1. **Set appropriate budget**: Balance precision vs performance
   ```cpp
   FlowDDA::setDefaultMaxBudget(10000);  // Lower for faster queries
   ```

2. **Use targeted clients**: Analyze only relevant pointers
   ```cpp
   dda.selectClient(DDAClientKind::Funptr);  // Faster than All
   ```

3. **Batch queries**: Leverage caching across multiple queries
   ```cpp
   for (auto *ptr : pointers) {
       auto pts = dda.getPointsTo(ptr);  // Later queries benefit from cache
   }
   ```

4. **Choose appropriate mode**: FlowDDA for most cases, ContextDDA when needed
   ```cpp
   dda.setDDAKind(DDAKind::FlowS_DDA);  // Faster
   ```

## SVFG Assumptions And Invariants

The DDA implementation assumes:

- Each pointer query can be mapped to an SVFG def node.
- Direct and indirect SVFG edge classes are consistent with `isDirectVFGEdge`
  and `isIndirectVFGEdge`, plus Lotus-specific extensions.
- Indirect edge guards (`edge->getPointsTo()`) represent object-sensitive flow.
- Constant/immutable objects are identified (`svfg->isConstantObject`) and
  skipped during indirect memory propagation.

## Key Files

- `include/Alias/DDA/DDAVFSolver.h`: generic solver algorithm and caches.
- `include/Alias/DDA/FlowDDA.h`, `lib/Alias/DDA/FlowDDA.cpp`: flow-sensitive mode.
- `include/Alias/DDA/ContextDDA.h`, `lib/Alias/DDA/ContextDDA.cpp`: context mode.
- `include/Alias/DDA/DDAClient.h`: query clients and candidate collection.
- `include/Alias/DDA/DDAPass.h`, `lib/Alias/DDA/DDAPass.cpp`: orchestration.
- `include/Alias/DDA/DDAStat.h`, `lib/Alias/DDA/DDAStat.cpp`: solver statistics.


## References

- Yulei Sui, Jingling Xue. "On-Demand Strong Update Analysis via Value-Flow Refinement". FSE'16.
- Yulei Sui, Jingling Xue. "Value-Flow-Based Demand-Driven Pointer Analysis for C and C++". TSE'18.
