# Monotone Dataflow Analysis Framework

A layered framework for monotone dataflow analyses in LLVM IR.

## Directory Structure

```
include/Dataflow/Mono/
├── Core/                          # Core abstractions (header-only)
│   ├── Problem.h                  # IntraMonoProblem & InterMonoProblem
│   ├── Domain.h                   # LLVMMonoAnalysisDomain
│   ├── CallStringContext.h        # CallStringCTX template
│   └── CallStringSolver.h         # Call-string interprocedural solver
├── Solver/                        # Fixpoint solvers (header-only)
│   ├── IntraSolver.h              # Intraprocedural solver
│   └── InterSolver.h              # Interprocedural solver
├── Container/                     # Container utilities (header-only)
│   ├── BitVectorSet.h             # Bit-vector optimized sets
│   └── Traits.h                   # Container abstractions
├── Support/                       # Support utilities (header-only)
│   ├── Result.h                   # DataFlowResult structures
│   ├── MonoDebug.h                # Debugging utilities
│   └── Soundness.h                # Soundness configuration
└── Analyses/                      # Analysis headers (implementations in lib/)
    ├── Intra/                     # Intra*.h — LiveVariables, ReachingDefinitions, etc.
    └── Inter/                     # Inter*.h — TaintAnalysis, ConstantPropagation, etc.
```

**Compiled vs header-only:** Core, Solver, Container, and Support are header-only (template-heavy framework). Analyses have both headers here and `.cpp` implementations in `lib/Dataflow/Mono/Analyses/`, which are compiled into the MONODataFlow library.

## Quick Start

### Define an Analysis

```cpp
#include "Dataflow/Mono/Core/Problem.h"
#include "Dataflow/Mono/Solver/IntraSolver.h"

class MyAnalysis : public mono::IntraMonoProblem<mono::ValueSetDomain> {
public:
  std::set<llvm::Value*> normalFlow(llvm::Instruction *Inst, 
                                     const std::set<llvm::Value*> &In) override {
    // Transfer function
  }
  
  std::set<llvm::Value*> merge(const std::set<llvm::Value*> &Lhs,
                                const std::set<llvm::Value*> &Rhs) override {
    std::set<llvm::Value*> Result = Lhs;
    Result.insert(Rhs.begin(), Rhs.end());
    return Result;
  }
  
  bool equal_to(const std::set<llvm::Value*> &Lhs,
                const std::set<llvm::Value*> &Rhs) override {
    return Lhs == Rhs;
  }
  
  std::unordered_map<llvm::Instruction*, std::set<llvm::Value*>> 
  initialSeeds() override {
    return {};
  }
};
```

### Run the Analysis

```cpp
MyAnalysis Problem(EntryPoints);
mono::IntraMonoSolver<mono::ValueSetDomain> Solver(Problem);
Solver.solve();
auto Results = Solver.getInResults();
```

## Architecture

### Core (`Core/`)
- **`IntraMonoProblem`**: Base class with `normalFlow()`, `merge()`, `equal_to()`, `initialSeeds()`
- **`InterMonoProblem`**: Extends with `callFlow()`, `returnFlow()`, `callToRetFlow()`
- **`LLVMMonoAnalysisDomain`**: LLVM IR type definitions (`Instruction*` nodes, `Value*` facts)
- **`CallStringContext`**: Bounded call-string context representation
- **`CallStringSolver`**: Call-string based interprocedural solver engine

### Solvers (`Solver/`)
- **`IntraSolver`**: Worklist-based intraprocedural fixpoint solver
- **`InterSolver`**: Interprocedural solver with context sensitivity

## API Notes

- `Core/IntraMonoSolver.h` is a compatibility shim that re-exports the
  authoritative implementation in `Solver/IntraSolver.h`.
- `CallStringInterProceduralDataFlowEngine::applyForwardFromSeeds()` returns
  `std::unique_ptr<ResultTy>`.
- Call-string engine callee/return resolution is driven by the provided ICFG
  (`getCalleesOfCallAt`, `getReturnSitesOfCallAt`, etc.), not a separate
  callee callback parameter.

### Containers (`Container/`)
- **`BitVectorSet.h`**: O(N/64) set operations for large universes (>100 elements)
- **`Traits.h`**: Container type traits and wrappers — `SetContainer` (std::set), `BitVectorContainer` (BitVectorSet), and domain helpers

### Analyses (`Analyses/`)
- **Intra** (`Intra*.h`): IntraLiveVariables, IntraReachingDefinitions, IntraAvailableExpressions, IntraConstantPropagation, IntraFullConstantPropagation, IntraUninitVariables, IntraReachable, IntraSolverTest
- **Inter** (`Inter*.h`): InterTaintAnalysis, InterConstantPropagation, InterFullConstantPropagation, InterSolverTest

## Conventions

- **Include guards**: Use `LOTUS_DATAFLOW_MONO_<SUBDIR>_<FILE>_H_` (e.g. `LOTUS_DATAFLOW_MONO_CORE_PROBLEM_H_`).

## Design Principles

- **Separation of Concerns**: Clear boundaries between problem, solver, and utilities
- **Layered Architecture**: Minimal dependencies between layers
- **Composability**: Mix and match components
- **Performance**: Optimized containers and efficient solvers

## Migration

Old includes:
```cpp
#include "Dataflow/Mono/DataFlow.h"
#include "Dataflow/Mono/Solver/IntraMonoSolver.h"
```

New includes:
```cpp
#include "Dataflow/Mono/Core/Problem.h"
#include "Dataflow/Mono/Solver/IntraSolver.h"
```

## Further Reading

See `Core/Problem.h`, `Solver/IntraSolver.h`, and `Analyses/` for detailed documentation and examples.
