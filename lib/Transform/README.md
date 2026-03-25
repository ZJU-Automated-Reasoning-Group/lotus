# Transform: LLVM IR Transformation Passes

Lotus provides a collection of LLVM IR transformation passes that normalize and simplify bitcode before running analyses (alias analysis, numerical abstract interpretation, symbolic abstraction) or to build light-weight optimization pipelines.

## Overview

These passes are used to prepare IR that is easier for analyses and tools to consume:

- **Core normalization**: Simplify PHI/select/constant expressions so downstream analyses don't need to handle every LLVM IR corner case
- **Memory and data canonicalization**: Make aggregates, GEPs, and global initializers easier to reason about
- **Control-flow cleanup**: Remove dead blocks, normalize loop latches, and give blocks stable names
- **Optimization helpers**: Light-weight inlining and loop/vector transforms used by optimization pipelines

## Core Normalization Passes

Normalization passes that rewrite IR into a simpler, analysis-friendly form.

- **`ElimPhi.cpp`**: Convert PHI nodes to explicit select or copy operations when possible
- **`ExpandAssume.cpp`**: Expand `llvm.assume` intrinsics into explicit branches and checks
- **`FlattenInit.cpp`**: Normalize complex global initializers into flat, explicit forms
- **`LowerConstantExpr.cpp`**: Lower `ConstantExpr` nodes into instructions so analyses can work over a uniform instruction set
- **`LowerSelect.cpp`**: Lower `select` instructions into equivalent branch-based control flow
- **`MergeReturn.cpp`**: Merge multiple `return` instructions into a single exit block

**Typical use cases**:
- Prepare bitcode for alias or numerical analyses that assume instruction-level IR
- Simplify unusual IR patterns produced by front-ends or optimizers
- Make control-flow and data-flow graphs easier to traverse

## Memory and Data Transforms

Transformations that simplify memory layout and aggregate operations.

- **`LowerGlobalConstantArraySelect.cpp`**: Lower selects over constant global arrays to simpler forms
- **`MergeGEP.cpp`**: Merge chained GEP instructions into a single GEP when possible
- **`SimplifyExtractValue.cpp`**: Simplify `extractvalue` instructions on aggregates
- **`SimplifyInsertValue.cpp`**: Simplify `insertvalue` instructions on aggregates
- **`CastElimPass.cpp`**: Eliminate redundant cast instructions

**Typical use cases**:
- Canonicalize pointer arithmetic and field accesses before pointer analysis
- Reduce the number of aggregate operations that downstream passes must understand
- Improve readability of IR when inspecting transformed modules

## Control-Flow Transforms

Transformations that restructure and clean up control flow.

- **`RemoveDeadBlock.cpp`**: Eliminate unreachable or trivially dead basic blocks
- **`RemoveNoRetFunction.cpp`**: Remove calls to known non-returning functions and tidy up unreachable code
- **`SimplifyLatch.cpp`**: Normalize loop latch structure to a canonical form
- **`NameBlock.cpp`**: Assign stable, human-readable names to basic blocks


**Typical use cases**:
- Cleanup after aggressive inlining or partial optimization
- Prepare IR for analyses that assume canonical loop shapes
- Improve the stability of analysis results and debug dumps

## Optimization and Pipeline Transforms

Passes that implement light-weight optimizations or orchestrate multiple transforms.

- **`UnrollVectors.cpp`**: Unroll short vector operations when profitable
- **`Unrolling.cpp`**: Loop unrolling transforms for selected loops

**Typical use cases**:
- Build an analysis-friendly optimization pipeline before running CLAM, SymAbsAI, or alias analyses
- Experiment with different levels of inlining and loop/vector transformations
- Replace floating-point operations in environments without hardware FP support

## Usage

### Basic Usage

```cpp
#include "Transform/LowerConstantExpr.h"

llvm::Module &M = ...;
LowerConstantExpr Pass;
bool Changed = Pass.runOnModule(M);
```

### Pipeline Usage

```cpp
#include "Transform/LowerSelect.h"
#include "Transform/MergeReturn.h"
#include "Transform/MergeGEP.h"

llvm::Module &M = ...;

LowerSelect SelectLowerer;
MergeReturn ReturnMerger;
MergeGEP GEPMerger;

bool Changed = 
    SelectLowerer.runOnModule(M) ||
    ReturnMerger.runOnModule(M) ||
    GEPMerger.runOnModule(M);
```

### As LLVM Passes

All passes are registered as standard LLVM passes and can be used with both legacy and new pass managers:

```bash
opt -load libTransform.so -lower-constant-expr -lower-select -merge-return <input.bc> -o <output.bc>
```

## Integration

These transforms are used by:

- **TPA**: IR normalization prepasses (GEP expansion, constant folding, etc.)
- **CLAM**: Numerical abstract interpretation pipeline
- **SymAbsAI**: Symbolic abstraction analysis pipeline
- **Alias Analysis**: Preprocessing passes for pointer analysis
- **ModuleOptimizer**: Driver pass that orchestrates multiple transforms

## See Also

- Headers: `include/Transform/`
- Documentation: `docs/source/transform/transforms.rst`
