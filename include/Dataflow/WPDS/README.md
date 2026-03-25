# WPDS-based Interprocedural Dataflow Analysis Framework

A framework for interprocedural may analyses using Weighted Pushdown Systems
(WPDS) over LLVM IR.

## Directory Structure

```
include/Dataflow/WPDS/
├── Core/                          # Core abstractions
│   ├── DataFlowFacts.h            # Fact domain (set of facts / environment)
│   ├── GenKillTransformer.h       # Semiring weight (gen/kill + relational flow)
│   ├── MemoryObjectFact.h         # Canonical memory-object helper layer
│   └── ExplodedWPDSBuilder.h      # Builder for exploded supergraph encoding
├── Solver/                        # Solver engine
│   └── InterProceduralDataFlowEngine.h  # WPDS solver, runs GPR algorithm
├── Clients/                       # Client analysis implementations
│   ├── WPDSTaintAnalysis.h
│   ├── WPDSConstantPropagation.h
│   ├── WPDSLivenessAnalysis.h
│   └── WPDSUninitializedVariables.h
├── Container/                     # Container utilities (placeholder)
├── Support/                       # Support utilities (placeholder)
└── InterProceduralDataFlow.h     # Umbrella header

lib/Dataflow/WPDS/
├── Core/
│   ├── DataFlowFacts.cpp
│   └── GenKillTransformer.cpp
├── Solver/
│   └── InterProceduralDataFlowEngine.cpp
├── Clients/
│   ├── WPDSTaintAnalysis.cpp
│   ├── WPDSConstantPropagation.cpp
│   ├── WPDSLivenessAnalysis.cpp
│   └── WPDSUninitializedVariables.cpp
└── CMakeLists.txt
```

## Architecture

### Core (`Core/`)
- **`DataFlowFacts`**: Fact domain representing a finite set of LLVM Values
- **`GenKillTransformer`**: May-analysis semiring weight implementing gen/kill-style flow functions for WPDS
- **`MemoryObjectFact`**: Minimal field-insensitive memory-object abstraction for facts and flow edges
- **`ExplodedWPDSBuilder`**: Template for building the paper's exploded supergraph encoding

### Solver (`Solver/`)
- **`InterProceduralDataFlowEngine`**: Encodes program supergraph as WPDS, runs forward/backward saturation (GPR), extracts results, supports explicit seeds, custom callee resolution, and external-call summaries

### Clients (`Clients/`)
Pre-built analyses using the WPDS framework:
- **`WPDSTaintAnalysis`**: Taint analysis
- **`WPDSConstantPropagation`**: Constant propagation (values NOT in set are constant)
- **`WPDSLivenessAnalysis`**: Live variable analysis
- **`WPDSUninitializedVariables`**: Uninitialized variable detection

## Supported Semantics

- The current framework targets distributive may analyses over a finite fact
  domain.
- `IN` and `OUT` are concrete fact sets at each instruction under the chosen
  seed/query automaton.
- `GEN` and `KILL` are the local transfer effects of the instruction, not
  accumulated path summaries.
- Memory objects are tracked via canonical base objects (allocas, globals,
  pointer arguments, pointer-returning calls) and pointer casts/GEPs collapse
  to that base.
- Indirect and external calls are handled conservatively and can be customized
  with `ExternalCallPolicy`.
- Precise field-sensitive updates, alias-aware strong updates, heap-shape
  recovery, and must analyses are out of scope for the current implementation.

## Quick Start

```cpp
#include "Dataflow/WPDS/InterProceduralDataFlow.h"

class MyAnalysis {
public:
    GenKillTransformer* createTransformer(llvm::Instruction* inst) {
        // Define gen/kill flow
    }
};

wpds::InterProceduralDataFlowEngine Engine;
auto Result = Engine.runForwardAnalysis(
    M,
    [](Instruction* inst) { return /* transformer */; },
    {/* initial facts */}
);

// Optional: restrict seeds to specific entries or exits.
auto EntryOnly = Engine.runForwardAnalysisFromEntries(
    M, createTransformer, {M.getFunction("helper")}, {/* initial facts */}
);

// Optional: customize unresolved-call handling.
wpds::InterProceduralDataFlowEngine::ExternalCallPolicy Policy;
Policy.buildSummary = [](llvm::CallBase *Call,
                         const std::vector<llvm::Value *> &PointerObjects,
                         const std::vector<llvm::GlobalValue *> &Globals) {
  return wpds::GenKillTransformer::one();
};
Engine.setExternalCallPolicy(Policy);
```

## References

- Reps, Schwoon, Jha: "Weighted Pushdown Systems and their Application to Interprocedural Dataflow Analysis" (SAS 2005)
- Lal, Reps: "Improving Pushdown System Model Checking" (CAV 2006)
- Lal, Reps, Balakrishnan: "Extended Weighted Pushdown Systems" (CAV 2005)

## Migration

Old includes:
```cpp
#include "Dataflow/WPDS/DataFlowFacts.h"
#include "Dataflow/WPDS/GenKillTransformer.h"
#include "Dataflow/WPDS/InterProceduralDataFlowEngine.h"
```

New includes:
```cpp
#include "Dataflow/WPDS/Core/DataFlowFacts.h"
#include "Dataflow/WPDS/Core/GenKillTransformer.h"
#include "Dataflow/WPDS/Solver/InterProceduralDataFlowEngine.h"
```

Or use the umbrella header:
```cpp
#include "Dataflow/WPDS/InterProceduralDataFlow.h"
```
