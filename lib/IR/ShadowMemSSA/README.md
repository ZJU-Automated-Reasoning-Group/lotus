# ShadowMem SSA

**ShadowMem SSA** queries the SSA-like shadow memory instructions inserted by the Sea-DSA ShadowMem pass. It is distinct from the sparse, AserPTA-backed MemorySSA built by `lib/IR/SVFG`.

ShadowMem SSA operates on top of Sea-DSA's ShadowMem instrumentation, which inserts special shadow function calls (e.g., `shadow.mem.load`, `shadow.mem.store`, `shadow.mem.arg.ref`) into the LLVM IR.

## Key Features

- **Shadow Instruction Parsing**: Extracts memory SSA information from Sea-DSA's shadow memory instructions
- **Interprocedural Memory Tracking**: Tracks memory regions across function boundaries through call sites and formal parameters
- **Def-Use Chain Analysis**: Provides APIs to query memory definitions and uses for program analysis
- **Call Site Analysis**: Analyzes memory regions accessed by callee functions at call sites
- **Formal Parameter Tracking**: Tracks memory regions corresponding to formal parameters of functions

## Shadow Memory Instructions

ShadowMem SSA works with shadow memory instructions inserted by Sea-DSA's ShadowMem pass:

**Intraprocedural Operations**:
- `shadow.mem.load(NodeID, TLVar, SingletonGlobal)` – Memory load (use)
- `TLVar shadow.mem.store(NodeID, TLVar, SingletonGlobal)` – Memory store (definition)
- `TLVar shadow.mem.arg.init(NodeID, SingletonGlobal)` – Initial value of formal parameter
- `TLVar shadow.mem.global.init(NodeID, TLVar, Global)` – Initial value of global

**Interprocedural Operations**:
- `shadow.mem.arg.ref(NodeID, TLVar, Idx, SingletonGlobal)` – Read-only input actual parameter
- `TLVar shadow.mem.arg.mod(NodeID, TLVar, Idx, SingletonGlobal)` – Modified input/output actual parameter
- `TLVar shadow.mem.arg.ref_mod(NodeID, TLVar, Idx, SingletonGlobal)` – Read-and-modified parameter
- `TLVar shadow.mem.arg.new(NodeID, TLVar, Idx, SingletonGlobal)` – Output actual parameter (newly created)
- `shadow.mem.in(NodeID, TLVar, Idx, SingletonGlobal)` – Input formal parameter
- `shadow.mem.out(NodeID, TLVar, Idx, SingletonGlobal)` – Output formal parameter

## Components

- **`ShadowMemSSA.cpp`**: Main implementation
  - `ShadowMemSSACallSite`: Represents a call site with its associated memory SSA information
  - `ShadowMemSSAFunction`: Gathers memory SSA-related input/output formal parameters
  - `ShadowMemSSACallsManager`: Manages memory SSA information for all functions and call sites

## Usage

```cpp
#include "IR/ShadowMemSSA/ShadowMemSSA.h"
#include "Alias/UnificationBased/seadsa/ShadowMem.hh"

// First, run ShadowMem pass to instrument the code
legacy::PassManager PM;
PM.add(new seadsa::ShadowMem(dsaAnalysis, asi));
PM.run(module);

// Query ShadowMem SSA information for the module
ShadowMemSSACallsManager mssaManager(module, pass, false /* only_singleton */);

// Analyze a call site
for (auto &F : module) {
  for (auto &I : instructions(&F)) {
    if (CallInst *CI = dyn_cast<CallInst>(&I)) {
      const ShadowMemSSACallSite *cs = mssaManager.getCallSite(CI);
      if (cs) {
        // Query memory regions accessed by the callee
        for (unsigned i = 0; i < cs->numParams(); ++i) {
          if (cs->isRef(i)) {
            // Parameter is read-only
          } else if (cs->isMod(i) || cs->isRefMod(i)) {
            // Parameter is modified
          }
        }
      }
    }
  }
}
```

## Integration

ShadowMem SSA is used by various analyses and optimizations in Lotus:

- **IPDeadStoreElimination**: Interprocedural dead store elimination pass
- **Memory Analysis**: Foundation for memory-aware analyses
- **Optimization Passes**: Enables memory-aware optimizations that reason about memory side effects

## See Also

- Headers: `include/IR/ShadowMemSSA/`
- Documentation: `docs/source/ir/shadowmemssa.rst`
