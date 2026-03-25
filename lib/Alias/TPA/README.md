# TPA: Flow- and Context-Sensitive Pointer Analysis

TPA is an **inclusion-based**, **flow- and context-sensitive** pointer analysis framework with k-limiting support. It uses a **semi-sparse** program representation to achieve both precision and scalability for large C/C++ programs.

## Key Features

- **Flow-sensitivity**: Respects program order and control flow
- **Context-sensitivity**: Distinguishes different calling contexts (with k-limiting support)
- **Semi-sparse representation**: Only analyzes relevant program points (def-use chains)
- **Type/offset-aware memory model**: Tracks objects with field/offset precision,
  with conservative summarization in cases such as arrays

## Architecture

```
LLVM IR → IR Transforms → Front-End Processing → Global Initialization → Semi-Sparse Analysis → Points-to Sets
```

## End-to-End Pipeline (What Happens Internally)

1. **IR normalization (`Transforms/`)**
   - Canonicalizes pointer-relevant IR forms (e.g., GEP/constant expr/byval/indirectbr)
   - Reduces edge cases so transfer functions operate on a smaller instruction surface

2. **Program construction (`FrontEnd/`, `Program/`)**
   - Builds `SemiSparseProgram` and per-function `CFG`
   - Keeps only pointer-relevant flow edges (`uses`/`succs`) for sparse propagation
   - Collects type/layout metadata (`TypeMap`, `PointerLayoutMap`, `ArrayLayoutMap`)

3. **Global bootstrap (`Analysis/GlobalPointerAnalysis`)**
   - Creates abstract pointers/objects for globals and functions
   - Seeds universal/null roots
   - Evaluates nested global initializers into initial `Env`/`Store`

4. **Fixpoint propagation (`Engine/`)**
   - `Initializer` seeds entry point + argv/envp roots
   - `TransferFunction` evaluates node semantics
   - `SemiSparsePropagator` updates `Memo` and worklist until convergence

5. **Query/output (`Analysis/`, `Output/`)**
   - Final top-level points-to queries are served from computed `Env`
   - `Memo` stores per-program-point incoming `Store` states used during propagation
   - Optional graph/text dumps for debugging and inspection

## Core Abstract Domains

- **`Env`**: top-level pointer bindings (`Pointer* -> PtsSet<MemoryObject*>`)
- **`Store`**: memory-cell contents (`MemoryObject* -> PtsSet<MemoryObject*>`)
- **`Memo`**: per-program-point incoming `Store` cache (join lattice)

This split is central to TPA's semi-sparse design: top-level updates can flow
without carrying store snapshots, while memory-level nodes join/propagate stores.

## Non-Obvious Design Choices

- **Two-level propagation**
  - Top-level nodes (`Alloc/Copy/Offset`) mostly mutate `Env`
  - Memory-level nodes (`Load/Store/Call/Ret`) consume and produce `Store`

- **Conservative unknown handling**
  - External/unknown pointer sources are mapped to `UniversalObject`
  - Keeps soundness when full program information is unavailable

- **Array modeling**
  - Arrays are often treated in a collapsed fashion through memory offset/layout policy
  - Precision/scale tradeoff is localized in memory-model helpers

- **Context sensitivity via `ContextPolicy`**
  - `KLimitContext` and `AdaptiveContext` provide controllable context growth
  - Keeps recursion/call-chain explosion bounded

## Directory Structure

- **`PointerAnalysis/`**: Core analysis implementation
  - `Analysis/`: Main analysis classes (`SemiSparsePointerAnalysis`, `GlobalPointerAnalysis`)
  - `Engine/`: Worklist propagation, transfer functions, store pruning
  - `FrontEnd/`: LLVM IR to internal representation conversion
  - `MemoryModel/`: Memory object and pointer management
  - `Context/`: Context sensitivity (`Context`, `KLimitContext`, `AdaptiveContext`)
  - `Program/`: Semi-sparse program representation
  - `Support/`: Data structures (`Env`, `Store`, `PtsSet`)

- **`Transforms/`**: LLVM IR normalization passes (GEP expansion, constant folding, etc.)

- **`Util/`**: Utilities (IO, data structures, iterators)

## Usage

### Command-Line Tool

```bash
tpa [options] <input bitcode file>
```

**Key options:**
- `-k-limit <n>`: Set k-limit for context-sensitive analysis (0 = context-insensitive)
- `-ext <file>`: External pointer table file
- `-print-pts`: Print points-to sets
- `-cfg-dot-dir <dir>`: Output CFG dot files

### Programmatic Usage

```cpp
SemiSparseProgramBuilder builder;
auto ssProg = builder.runOnModule(module);

SemiSparsePointerAnalysis analysis;
analysis.runOnProgram(ssProg);

const llvm::Value *some_ptr_value = /* pointer-typed LLVM value */;
auto ptsSet = analysis.getPtsSet(some_ptr_value);
```

## Analysis Characteristics

| Characteristic | Value |
|----------------|-------|
| **Analysis Type** | Inclusion-based (Andersen-style) |
| **Flow-Sensitive** | ✅ Yes |
| **Context-Sensitive** | ✅ Yes (with k-limiting) |
| **Field-Sensitive** | Partial (type/offset-aware, arrays may be summarized) |
| **Representation** | Semi-sparse |

## See Also

- Parent README: `lib/Alias/README.md`
- Documentation: `docs/source/alias/tpa.rst`
