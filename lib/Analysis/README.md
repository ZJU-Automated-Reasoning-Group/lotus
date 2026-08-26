# Analysis Library

Core analyses built on LLVM IR.

| Subdir | Purpose |
|--------|---------|
| **CFG** | Reachability, dominators, post-dominators, topological order, back edges, code metrics. |
| **ControlDependence** | Standard CD, NTSCD variants, DOD variants, strong control closure, and Lotus ICFG integration. |
| **DebugInfo** | MetadataManager, LoopStructure, debug-info-driven annotations. |
| **FeatureExtraction** | Memory-related feature extraction using Sea-DSA for ML-oriented analysis workloads. |
| **Loop** | Loop forest/structure, dependence graphs, SCC DAGs, invariants, induction variables, loop-carried dependences, iteration-space and memory-cloning analyses. See `Loop/README.md`. |
| **Multiplicity** | Allocation multiplicity classification for globals, stack allocations, heap allocations, and call-site summaries. |
| **NullPointer** | Null-check, null-flow, null-equivalence; context-sensitive variants. |
| **ParameterSummary** | Parameter effect summaries and resource-table support for interprocedural parameter modeling. |
| **Profile** | Profile-guided hotness analysis utilities. |
| **Purity** | Function purity analysis, summary storage, attribute inference, and unknown-impact modeling. |
| **SCCP** | Sparse conditional constant propagation analysis and related support. |
| **TypeHierarchy** | C++ class hierarchy, vtable reconstruction, virtual-call resolution. |

Security-oriented side-channel analyses and transformations now live under
`lib/Security`.
