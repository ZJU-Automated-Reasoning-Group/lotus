# IFDS/IDE Dataflow Analysis

## Directory Layout

### include/Dataflow/IFDS/

- **Core/** – Core framework headers:
  - **IFDSFramework.h** – Core framework (path edges, summary edges, problem interfaces).
  - **IFDSIDESolverConfig.h** – Solver configuration (e.g. `follow_returns_past_seeds`, `record_edges`).
  - **IFDSIDESolverStatistics.h** – Solver statistics tracking.
  - **EdgeFunctionCache.h** – Edge function memoization.
  - **SolverGraphContext.h** – Shared ICFG/callgraph/successor/seed construction.
  - **SolverRunState.h** – Shared monotonic path/summary edge run-state containers.
- **Solvers/** – Header-only solver implementations:
  - **IFDSSolver.h** / **IFDSSolver.tpp** – Sequential IFDS tabulation solver.
  - **IDESolver.h** / **IDESolver.tpp** – IDE solver.
  - **PathAwareIFDSSolver.h** – Path-tracking IFDS solver.
  - **PathAwareIDESolver.h** – Path-tracking IDE solver.
- **Utils/** – Utility headers:
  - **LLVMFlowHelpers.h** – LLVM-specific flow function helpers.
- **Clients/** – Analysis problem definitions (IFDSTaintAnalysis, IDEConstantPropagation, etc.).

### Solver features (aligned with Phasar where applicable)

- **Multiple return sites**: Each call can have several return sites (e.g. normal and unwind for `invoke`). The solver uses all CFG successors of the call.
- **Unbalanced returns**: When `follow_returns_past_seeds()` is enabled, returns from functions that had no incoming call edge (e.g. entry-point returning) are still propagated to all callers' return sites with the zero fact.
- **SSA-style result API**: `get_facts_at_in_llvm_ssa(inst)` (IFDS) and `get_value_at_in_llvm_ssa(inst, fact)` (IDE) return results at the successor instruction(s) where the defined value is valid (for non-void instructions).
- **Caching**: Flow function results (normal, call-to-return) and edge function lookups (normal, call-to-return) are cached to avoid recomputation.
- **Shared graph context**: IFDS and IDE solvers use a common graph/seed construction layer to avoid drift.
- **Path-aware hooks**: Path-aware IDE/IFDS solvers consume explicit path/summary edge hooks from the base IDE solver.

### lib/Dataflow/IFDS/

- **Debug/** – Framework debug helpers (e.g. IFDSDebugUtils).
- **Clients/** – Concrete analyses built on the framework.

## Writing An Analysis

*Use IFDS**, if
- your analysis is a plain reachability problem, that is, a data-flow fact can either hold, or not.
- your analysis problem is distributive, that is, within a flow function the reachability of a successor fact may depend on at most one incoming fact.
- your analysis problem is a may-analysis, since IFDS always uses set-union as merge operator

All bit-vector problems fall into this category, including all classical gen-kill problems. Examples are taint analysis, uninitialized-variables analysis, constness analysis, etc.


*Use IDE**, if

- your analysis computes environments that associate the holding data-flow facts with an additional computed value
- OR the set of holding data-flow facts is structured, i.e. some facts subsume other facts
- OR your analysis is a must-analysis, since in contrast to IFDS the merge operator is customizable
- your analysis problem is distributive, that is, within a flow function the reachability a of a successor fact as well as its associated value may depend on at most one incoming fact/value.

Examples are linear constant-propagation, typestate analysis, type analysis, feature-taint analysis, etc.

## Related Work


You may also refer to https://github.com/secure-software-engineering/phasar/wiki/Useful-Literature

- ISSTA 23: Reducing the Memory Footprint of
IFDS-Based Data-Flow Analyses Using Fine-Grained Garbage Collection. Dongjie He, Yujiang Gui, Yaoqing Gao, and Jingling Xue.
- ICSE 21: Sustainable Solving: Reducing The Memory Footprint of IFDS-Based Data
Flow Analyses Using Intelligent Garbage Collection. Steven Arzt.
- ASE 20: Performance-Boosting Sparsification of the IFDS Algorithm with
Applications to Taint Analysis. Dongjie He, Haofeng Li, Lei Wang, Haining Meng, Hengjie Zheng, Jie Liu, Shuangwei Hu, Lian Li, and Jingling Xue.
- TACAS 19: PhASAR: An Inter-procedural Static Analysis Framework for C/C++. Philipp Dominik Schubert, Ben Hermann, and Eric Bodden.
- ECOOP 16: Boomerang: DemandDriven Flow- and Context-Sensitive Pointer Analysis for Java. Johannes Späth, Lisa Nguyen Quang Do, Karim Ali, and Eric Bodden.
- ICSE 16: StubDroid: Automatic Inference of Precise Data-Flow Summaries for the Android Framework. Steven Arzt and Eric Bodden.
- ICSE 15: Database-Backed Program Analysis for Scalable Error Propagation. Cathrin Weiss, Cindy Rubio-González, and Ben Liblit.
- ICSE 14: Reviser: Efficiently Updating IDE-/IFDS-Based Data-Flow Analyses in Response to Incremental Program Changes. Steven Arzt and Eric Bodden.
- PLDI 14: FlowDroid: Precise Context,
Flow, Field, Object-Sensitive and Lifecycle-Aware Taint Analysis for Android App. Steven Arzt, Siegfried Rasthofer, Christian Fritz, Eric Bodden, Alexandre Bartel, Jacques
Klein, Yves Le Traon, Damien Octeau, and Patrick D. McDaniel.
- CC 10: Practical Extensions to the IFDS Algorithm. Nomair A Naeem, Ondřej Lhoták, and Jonathan Rodriguez.
- CC 08: IDE Dataflow Analysis in the Presence of Large Object-Oriented Libraries. Atanas Rountev, Mariana Sharp, and Guoqing Xu.
- CAV 05: Extended Weighted Pushdown Systems. Akash Lal, Thomas Reps, and Gogul Balakrishnan.
- SAS 03: Weighted Pushdown Systems and Their Application to Interprocedural Dataflow Analysis. Thomas Reps, Stefan Schwoon, and Somesh Jha.
- Precise Interprocedural Dataflow Analysis with Applications to Constant Propagation
- POPL 95: Precise Interprocedural Dataflow Analysis via Graph Reachability. Thomas Reps, Susan Horwitz, and Mooly Sagiv.
