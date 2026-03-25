# SymAbsAI – Symbolic Abstraction + Abstract Interpretation

## Overview

This is a complete **program analysis framework** for LLVM IR using abstract interpretation with symbolic abstraction. It provides a full analysis infrastructure including fixpoint engines, abstract domains, and LLVM pass integration.

**⚠️ Important Distinction**: This framework is **NOT** the same as `lib/Solvers/SMT/SymAbs`, which provides formula-level abstraction algorithms for SMT formulas.

### Key Differences

| Aspect | `lib/Verification/SymAbsAI` (this framework) | `lib/Solvers/SMT/SymAbs` |
|--------|--------------------------------------------------------|--------------------------|
| **Input** | LLVM IR (program code) | SMT bit-vector formulas (Z3 expressions) |
| **Output** | Abstract domain values for LLVM values | Abstract constraints (intervals, octagons, etc.) |
| **Approximation** | Works directly with program semantics | Converts bit-vectors to linear integer formulas |
| **Level** | Program-level abstract interpretation | Formula-level abstraction algorithms |
| **Integration** | Integrated LLVM pass with fixpoint engine | Standalone SMT formula processing |
| **Use Case** | Static analysis and optimization of programs | Abstracting SMT formulas for constraint solving |

## Architecture

### Core Components

- **Analyzer** – Fixpoint engine that drives abstract interpretation over a function
- **FragmentDecomposition** – Partitions CFG into acyclic fragments for scalable analysis
- **DomainConstructor** – Factory for creating and composing abstract domains
- **FunctionContext** – Per-function analysis context and state management
- **ModuleContext** – Module-level context for interprocedural setup
- **SymAbsAIPass** – LLVM function pass that integrates SymAbsAI into optimization pipelines
- **AbstractValue** – Base interface for abstract domain values
- **InstructionSemantics** – Converts LLVM instructions to SMT expressions

### Abstract Domains

These domains work directly on LLVM IR values (via `RepresentedValue`):

- **NumRels** – Numerical relations (e.g., `x <= y + 5`)
- **Intervals** – Value range analysis (e.g., `x ∈ [0, 100]`)
- **Affine** – Affine relationships (e.g., `y = 2*x + 3`)
- **BitMask** – Bit-level tracking and alignment
- **SimpleConstProp** – Constant propagation
- **Boolean** – Boolean truth values and invariants
- **Predicates** – Path predicates and assertions
- **MemRange** – Memory access bounds in terms of function arguments
- **MemRegions** – Memory region and pointer analysis
- **Congruence** – Modular arithmetic constraints
- **Zones** – Difference bound matrices (DBM)
- **Octagon** – Octagonal constraints (±x ± y ≤ c)

## Typical Use Cases

- Constant propagation and dead code elimination
- Bounds checking and array access verification
- Bit-level analysis and alignment tracking
- Numerical invariant discovery
- Memory safety analysis
- Custom abstract interpretation passes

## Usage

### As an LLVM Pass

```cpp
#include "Verification/SymAbsAI/Core/SymAbsAIPass.h"

// The pass can be registered and run in LLVM optimization pipelines
```

### Programmatic Usage

```cpp
#include "Verification/SymAbsAI/Analyzers/Analyzer.h"
#include "Verification/SymAbsAI/Core/FunctionContext.h"

// Create function context
auto fctx = mctx->createFunctionContext(&function);

// Create analyzer with abstract domains
DomainConstructor domain = ...;
auto analyzer = Analyzer::New(*fctx, fragment_decomp, domain);

// Run analysis
analyzer->analyze();
```

## When to Use This Framework

Use `lib/Verification/SymAbsAI` when:
- You're analyzing LLVM IR programs
- You need a complete abstract interpretation framework
- You want to integrate analysis into LLVM optimization passes
- You need fixpoint computation over program control flow

Use `lib/Solvers/SMT/SymAbs` when:
- You have SMT formulas (bit-vectors) that need abstraction
- You need to approximate bit-vector constraints with linear integer constraints
- You're working at the formula/solver level, not the program level

