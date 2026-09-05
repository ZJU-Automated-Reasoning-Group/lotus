Major Components Overview
==========================

This page consolidates the high-level component inventory that used to live
in ``README.md``. Each section links to the dedicated documentation page where
you can find deeper usage guides and configuration details.

Alias Analysis
--------------

See :doc:`../alias/alias_analysis` for detailed instructions and command examples.

* **AllocAA** – Lightweight alias analysis built from simple heuristics for
  allocation tracking.
* **DyckAA** – Unification-based exhaustive alias analysis
  (``lib/Alias/UnificationBased/DyckAA``).
* **CFL (via LLVM)** – Context-Free Language alias analysis from LLVM (used by the alias wrapper).
* **Sea-DSA** – Context-sensitive and field-sensitive analysis with
  Sea-DSA (``lib/Alias/UnificationBased/seadsa``). Does not require Boost.
* **SparrowAA** – Inclusion-based pointer analysis without on-the-fly
  call-graph construction (``lib/Alias/InclusionBased/SparrowAA``).
* **FPA** – Function Pointer Analysis toolbox (FLTA, MLTA, MLTADF, KELP) under
  ``lib/Alias/Specialized/FPA`` for resolving indirect calls.
* **DynAA** – Dynamic checker living in ``tools/alias/dynaa`` that validates static
  alias analyses against runtime traces.
* **AserPTA** – Constraint-based pointer analysis with multiple context
  sensitivities, including k-callsite and k-origin (thread-creation)
  sensitivity (``lib/Alias/InclusionBased/AserPTA``).
* **LotusAA** – Native alias analysis engine with interprocedural,
  flow-sensitive, and field-sensitive reasoning
  (``lib/Alias/InclusionBased/LotusAA``). See :doc:`../alias/lotusaa`.
* **FlowSensitivePTA** – Sparse flow-sensitive inclusion-based pointer
  analysis with an object-versioned solver variant
  (``lib/Alias/InclusionBased/FlowSensitive``).
  See :doc:`../alias/flowsensitive`.
* **DDA** – Demand-driven alias-analysis infrastructure that refines alias
  information on demand instead of materializing a full global solution
  (``lib/Alias/DemandDriven/DDA``). See :doc:`../alias/dda`.
* **SRAA** – Strict Relations Alias Analysis that proves pointers cannot alias
  by establishing strict relations between symbolic expressions
  (``lib/Alias/Specialized/SRAA``). See :doc:`../alias/sraa`.
* **TPA** – Inclusion-based, flow- and context-sensitive pointer analysis with
  k-limiting support and a semi-sparse program representation
  (``lib/Alias/InclusionBased/TPA``). See :doc:`../alias/tpa`.
* **Spec** – Alias specification manager that loads and serves per-function
  specifications for library routines (``lib/Alias/Infrastructure/Spec``).
  See :doc:`../alias/spec`.
* **Metrics** – Pointer analysis metrics for measuring precision and
  soundness-related properties (``lib/Alias/Infrastructure/Metrics``).
  See :doc:`../alias/metrics`.
* **TypeQualifier** – Qualifier-based analysis infrastructure that models
  qualifier-style properties over program values
  (``lib/Alias/Specialized/TypeQualifier``). See :doc:`../alias/typequalifier`.

Intermediate Representations
----------------------------

See :doc:`../ir/index` for builder APIs and code snippets.

* **Program Dependence Graph (PDG)** – Captures fine-grained data/control
  dependencies.
* **Static Single Information (SSI)** – Planned extension of SSA to encode
  predicate information.
* **DyckVFG** – Value Flow Graph variant designed for Dyck-based alias
  analyses (``lib/Alias/UnificationBased/DyckAA/DyckVFG.cpp``).
* **SVFG** – Sparse Value-Flow Graph combining SSA with MemorySSA for
  efficient interprocedural value-flow analysis (``lib/IR/SVFG``).
  See :doc:`../ir/svfg`.
* **GVFG** – Guarded Value-Flow Graph, a per-function IR for value flow,
  memory flow, and path-sensitive dependencies (``lib/IR/GVFG``).
  See :doc:`../ir/gvfg`.
* **ICFG** – Interprocedural Control Flow Graph extending the CFG with call
  and return edges (``lib/IR/ICFG``). See :doc:`../ir/icfg`.
* **ShadowMemSSA** – Query layer for SSA-like shadow memory instructions
  inserted by the Sea-DSA ShadowMem pass (``lib/IR/ShadowMemSSA``).
  See :doc:`../ir/shadowmemssa`.
* **GSA** – Gated SSA, a read-only view of the control flow guarding SSA
  values at join points (``lib/IR/GSA``). See :doc:`../ir/gsa`.
* **vSSA** – Variable Static Single Assignment, an SSI variant designed for
  precise symbolic range analysis (``lib/IR/vSSA``). See :doc:`../ir/vssa`.

Machine Learning Features
------------------------

See :doc:`../ml/index` for ML feature extraction APIs.

* **CanaryML** – Memory-related feature extraction using Sea-DSA for ML applications
  (``lib/Analysis/FeatureExtraction/``). Provides ``MemoryMLFeaturesPass`` for extracting memory access
  patterns and structural features from call sites, useful for training memory
  safety predictors.

Abstract Interpretation
-----------------------

See :doc:`../verification/clam` for CLAM and :doc:`../verification/symabs-ai`
for higher-level abstractions.

* **CLAM** – Modular AI-driven static analyzer with multiple abstract domains
  (``tools/verifier/clam`` and ``third-party/verification/clam``).
* **SymAbsAI** – Configurable abstract interpretation framework with domain
  composition (``lib/Verification/SymAbsAI`` and ``include/Verification/SymAbsAI``).

Symbolic Automata
-----------------

See :doc:`../verification/seal` for details.

* **Seal** (vendored, opt-in) — Symbolic automata lifter for stateful software
  systems. Builds finite-state-machine models from LLVM IR by combining loop
  summary analysis, symbolic execution, and abstract interpretation.
  Published at CAV 2026.

Symbolic Execution
------------------

The SymbolicExecution subsystem is a top-level engine under
``lib/SymbolicExecution`` and ``include/SymbolicExecution``. It performs
path-sensitive symbolic execution over the guarded value-flow graph, tracks
symbolic scalar and memory facts, and uses SMT-backed path-condition checks for
feasibility. The ``lotus-check --engine=symex`` frontend invokes this engine for
symbolic-execution bug checks. See :doc:`../symbolic_execution/index` for the
engine documentation.

Utilities and Reachability
--------------------------

See :doc:`../utils/index` and :doc:`../cfl/index` for extended guides.

* **cJSON** – Lightweight JSON parser (``include/Utils/Formats/cJSON.h``).
* **Transform** – LLVM bitcode transformation passes housed in ``lib/Transform``.
* **CFL Reachability** – General-purpose CFL reachability utilities and
  tooling (``tools/cfl``)
