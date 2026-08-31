# IR Library

IR representations and transformations for LLVM.

| Subdir | Purpose |
|--------|---------|
| **GSA** | Gated SSA (gamma nodes, control dependence). Thinned GSA, optional PHI replacement. |
| **GVFG** | Guarded Value-Flow Graph for per-function value, memory, and path-sensitive dependencies. |
| **ICFG** | Interprocedural CFG: call/return edges, call graph. Used by IFDS/IDE, WPDS, PDG. |
| **PDG** | Program dependence graph. Data/control deps, slicing, context-sensitive slicing. |
| **ShadowMemSSA** | Query API over Sea-DSA ShadowMem instrumentation for interprocedural memory tracking. |
| **SSI** | Static Single Information. Sigma functions, dual dominance, path-sensitive representation. |
| **SVFG** | AserPTA-backed sparse MemorySSA and value-flow graph. |
| **vSSA** | Value SSA construction and related SSA normalization utilities. |
