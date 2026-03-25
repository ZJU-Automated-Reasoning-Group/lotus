# Sifa (Verification/Sifa)

C++/LLVM port of **Ultimate Library-Sifa** (Symbolic Interpretation with Fluid Abstractions) for lotus, aligned with `ultimate-0.3.1/trunk/source/Library-Sifa`.

## Algorithm overview

Sifa implements the symbolic interpretation engine described in:

> *D. Dietsch, M. Heizmann, A. Nutz, C. Schätzle, F. Schüssele. Ultimate Taipan with Symbolic Interpretation and Fluid Abstractions (Competition Contribution). TACAS 2020, LNCS 12079, pp. 418–422.*

The approach is inspired by **Algebraic Program Analysis** (Tarjan, Brzozowski) and **Logical Interpretation** (Tiwari & Gulwani). It consists of two main components:

1. **ICFG interpreter** — For a (partial) ICFG and a subset of program locations (LOIs), generates path expressions represented as **RegexDAGs**. A RegexDAG is a directed acyclic graph whose vertices are labeled with regular expressions over transitions (with summary and enter statements for interprocedural paths). Each RegexDAG has a sink representing a location of interest.

2. **DAG interpreter** — Analyzes a RegexDAG in topological order by applying three operators:
   - **Post operator** — Computes strongest postcondition for star-free regular expressions; optionally applies an abstraction function (controlled by fluids).
   - **Call summarization** — Computes a summary for procedure calls (with or without context).
   - **Loop summarization** — Computes a summary for the Kleene-star operator via fixpoint iteration, resolving nested loops by recursively inserting summaries.

When a vertex has multiple incoming edges, the input states are joined (logical disjunction). The **fluid** abstraction policy decides when to apply abstraction to avoid blow-up; different heuristics (NeverFluid, SizeLimitFluid, etc.) can be swapped.

## Abstract domains (include/Verification/Sifa/Domain/)

Domain implementations are Ultimate-aligned and follow the same roles as in Ultimate's Library-Sifa and Sifa plugin.

### Domain choice (convenience APIs)

The user chooses the domain by calling the corresponding API or by switching on **SifaOptions::domainKind** in their code:

| Domain | API | State type |
|--------|-----|------------|
| **Reachability** | `isReachable()`, `isReachableInterprocedural()` | `bool` |
| **Interval** | `analyzeToWithIntervalDomain()` | `IntervalState` |
| **Octagon** | `analyzeToWithOctagonDomain()` | `OctagonState` |
| **Eq** | `analyzeToWithEqDomain()` | `EqState` |
| **ExplicitValue** | `analyzeToWithExplicitValueDomain()` | `ExplicitValueState` |

Example: choose at run time using `SifaOptions::domainKind`:

```cpp
switch (options.domainKind) {
  case SifaDomainKind::Reachability:
    return isReachable(F, target, options);
  case SifaDomainKind::Interval:
    return analyzeToWithIntervalDomain(F, target, IntervalState(false), options);
  case SifaDomainKind::Octagon:
    return analyzeToWithOctagonDomain(F, target, OctagonState(false), options);
  // ...
}
```

### Other domain types

- **CompoundDomain**, **StatsWrapperDomain** – Use the template `analyzeTo<StateT>(..., domain, options)` with a constructed domain.
- **StateBasedDomain**, **RelationCheckUtil**, **TermToInterval**, **DnfToExplicitValue** – Supporting types used by the above domains.

## Bitcode Support (C/C++ roadmap)

The primary “real LLVM IR” entry point is `lotus::sifa::analyzeSymAbs*()` (see `include/Verification/Sifa/SifaSymAbs.h`), which runs Sifa with a SymAbsAI-backed abstract domain.

### Supported subset (strict mode)

By default, `SifaSymAbsOptions::validateLlvmSubset` is enabled. The current *well-defined* supported subset is:

- LLVM IR compatible with lotus’ LLVM build (SymAbsAI currently targets LLVM 14).
- Scalar integers (`i1`…`i64`) and pointers.
- Control-flow: `br`, `switch`, `phi`, `select`, `ret`.
- Scalar ops: integer arith/bitwise (`add/sub/mul/div/rem/shifts/and/or/xor`), casts (`zext/sext/trunc`, `ptrtoint/inttoptr/bitcast`), `icmp`.
- Memory operations are allowed structurally (`alloca`, `load`, `store`, `getelementptr`), but precision depends on the chosen abstract domain (see below).

Unsupported in the strict subset (will raise `std::invalid_argument`):

- Vectors and vector instructions.
- First-class aggregate values (e.g. struct-typed SSA values / `extractvalue` / `insertvalue`).
- Exceptions/EH (`invoke`, landingpads, funclets, `resume`, …).
- Atomics/fences.
- Varargs (`va_arg`).
- `float` (and `fptrunc`/`fpext`). `double` is optional via `SifaSymAbsOptions::allowDouble`.

### Practical C/C++ guidance

For a usable “C/C++ bitcode” workflow today, a good starting point is:

- Compile without exceptions and atomics if you want strict validation to pass.
- Prefer IR that is close to SSA for scalar locals (e.g. compile with optimizations or run `mem2reg`), otherwise most domains will treat loads/stores conservatively.

### Per-block transfer strategy (precision-performance trade-off)

You can choose **instruction-by-instruction** (more precise, slower) or **block-wise** (fast havoc, less precise) per basic block via **SifaOptions::blockTransferPolicy**:

- **BlockTransferPolicy**: set of blocks (or a predicate) that use block-wise transfer; all others use instruction-by-instruction.
- **Instruction-wise** (default): `applyBlockTransfer(bb, in)` — full semantics per instruction.
- **Block-wise**: `applyBlockWiseHavoc(bb, in)` — treat block as black box (havoc defined values); sound but less precise.

Example: use block-wise for hot or large blocks to speed up analysis.

```cpp
BlockTransferPolicy policy;
policy.addBlockWise(&someBasicBlock);
SifaOptions options;
options.blockTransferPolicy = policy;
auto state = analyzeToWithIntervalDomain(F, target, IntervalState(false), options);
```

### Instruction-level block transfer (soundness)

The value domains (Interval, Eq, ExplicitValue) apply **real instruction-level transfer** on CFG edges so that `post(Edge)` models program semantics (sound over-approximation):

- **IntervalDomain**: Full block transfer in `lib/Verification/Sifa/Domain/IntervalDomain.cpp` — binary ops (add/sub/mul/div/rem/shifts/and/or/xor), casts (trunc/zext/sext), icmp, select, phi; call/gep yield top. With **SifaOptions::aliasAnalysis**, load/store/alloca use **region-based memory**.
- **EqDomain**: Copy/phi/select equality propagation (unite result with operands); other instructions ensure the result is in the state. With **SifaOptions::aliasAnalysis**, load/store/alloca use **region-based memory** (content = representative value, load unites with content).
- **ExplicitValueDomain**: Constant propagation over instructions (constants, arithmetic, casts, phi, select). With **SifaOptions::aliasAnalysis**, load/store/alloca use **region-based memory** (content = constant or top).
- **OctagonDomain**: Block transfer in `lib/Verification/Sifa/Domain/OctagonDomain.cpp` — copy/constant/affine (res = src, res = c, res = src + k); phi/select/non-linear ops havoc the result. With **SifaOptions::aliasAnalysis**, load/store/alloca use **region-based memory** (memory content stored as intervals, load result constrained to joined interval).

### Region-based memory (lib/Alias, IKOS/CLAM style)

When you pass an **alias analysis** via **SifaOptions::aliasAnalysis**, **all value domains** (Interval, Octagon, Eq, ExplicitValue) use a **region-based memory model** that reuses pointer/alias analyses from **lib/Alias**. For a richer, CLAM-aligned model (one region per pointer, type info, field-sensitive), see **lib/Verification/clam** (HeapAbstraction, SeaDsa).

- **Regions**: allocas in the function + globals in the module (see `include/Verification/Sifa/RegionMemory.h`).
- **Resolve pointer**: Uses `AliasAnalysisWrapper::getPointsToSet()` when the backend supports it (e.g. SparrowAA); otherwise `mayAlias(ptr, region)` over all regions.
- **Load**: Result = join of abstract values stored in the regions the pointer may point to (sound over-approximation).
- **Store**: For each region the pointer may alias, join the stored value with the value being stored.
- **Alloca**: Region is initialized to top; the alloca instruction result remains top (pointer not tracked as value).

This matches the “Option 2” style used in IKOS and CLAM: one abstract cell per region, with AA used to resolve pointers. Any backend from **lib/Alias** (SparrowAA, AllocAA, DyckAA, CFL via LLVM, SeaDsa, TPA, etc.) can be used; precision depends on the AA.

Example: run interval analysis with region memory using SparrowAA.

```cpp
#include "Verification/Sifa/Sifa.h"
#include "Alias/AliasAnalysisWrapper/AliasAnalysisWrapper.h"

lotus::AAConfig aaConfig = lotus::AAConfig::SparrowAA_NoCtx();
auto AA = lotus::AliasAnalysisFactory::create(M, aaConfig);
lotus::sifa::SifaOptions options;
options.aliasAnalysis = AA.get();
auto state = lotus::sifa::analyzeToWithIntervalDomain(F, target, lotus::sifa::IntervalState(false), options);
```

### Domains and precision (memory)

The domain string (`SifaSymAbsOptions::abstractDomain`) controls what information is tracked precisely. For programs that use memory heavily (typical unoptimized C/C++), consider including a memory-aware domain (e.g. `MemRange` / `ValidRegion`) instead of only numeric domains like `Interval, Octagon`. For native Sifa value domains (e.g. Interval), set **SifaOptions::aliasAnalysis** to enable region-based memory via lib/Alias.

### Comparison with CLAM (lib/Verification/clam)

CLAM uses a **HeapAbstraction** (see `include/Verification/clam/HeapAbstraction.hh`) that maps **(function, pointer) → one Region** with a unique **RegionId**, **RegionInfo** (type: INT_REGION, BOOL_REGION, PTR_REGION, UNTYPED; bitwidth; is_sequence, is_heap, is_cyclic), and optional **singleton Value\***. Load/Store in CLAM call `getRegion(mem, regions, params, I, ptr)` and get **one** region; if unknown they handle conservatively. The default implementation is **SeaDsaHeapAbstraction** (SeaDsa), which builds regions from SeaDsa’s graph (nodes/fields). CLAM also supports **getRegion(F, V, offset, AccessedType)** for field-sensitive access.

Sifa’s region model is simpler and AA-driven:

| Aspect | CLAM (lib/Verification/clam) | Sifa (RegionMemory.h + lib/Alias) |
|--------|------------------------------|------------------------------------|
| Region set | From HeapAbstraction (SeaDsa): nodes/fields, unique RegionId | Fixed set: **allocas** in function + **globals** in module (Value* as id) |
| Pointer resolution | **One** region per pointer via `getRegion(F, ptr)` (or unknown) | **Set** of regions via getPointsToSet or mayAlias; **join** over set for Load, **join into each** for Store |
| Type info | RegionInfo (type, bitwidth, is_sequence, is_heap) | None (all regions treated uniformly; value domain tracks scalar type) |
| Field-sensitive | Yes: `getRegion(F, V, offset, AccessedType)` | No (one cell per alloca/global) |
| Backend | SeaDsa (HeapAbstraction) | Any **lib/Alias** backend (SparrowAA, AllocAA, DyckAA, CFL via LLVM, SeaDsa, TPA, …) |

Both designs are **sound** (over-approximate). CLAM’s model is more precise when SeaDsa gives a single region and type/offset info; Sifa’s model is lightweight and works with any AA. To use a CLAM-style heap abstraction from Sifa you would need an adapter that implements “pointer → set of regions” (e.g. one region from HeapAbstraction when not unknown) and maps RegionId to Sifa’s memory map; that could live in a separate integration layer.

### Roadmap

Key next steps to broaden “C/C++ bitcode” coverage:

- `float` support (and `fptrunc`/`fpext`) in `FloatingPointModel`.
- First-class aggregates / struct-typed SSA values (or a stricter “memory-only aggregates” discipline + checks).
- Exceptions/EH and atomics if needed for target workloads.
