# Analysis Library

Core analyses built on LLVM IR.

| Subdir | Purpose |
|--------|---------|
| **CFG** | Reachability, dominators, post-dominators, topological order, back edges, code metrics. |
| **Concurrency** | MHP, happens-before, lock sets, escape/thread sharing, memory use-def, join-target. See `Concurrency/README.md`. |
| **Crypto** | CT-LLVM: constant-time side-channel analysis (ECOP 24 CtChecker–related). |
| **DebugInfo** | MetadataManager, LoopStructure, debug-info–driven annotations. |
| **NullPointer** | Null-check, null-flow, null-equivalence; context-sensitive variants. |
| **Spectre** | Cache modeling, speculative-execution analysis, cache-timing side channels. |
| **TypeHierarchy** | C++ class hierarchy, vtable reconstruction, virtual-call resolution. |
