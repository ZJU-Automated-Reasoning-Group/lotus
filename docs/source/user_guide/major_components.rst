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
  (``lib/Alias/DyckAA``).
* **CFL (via LLVM)** – Context-Free Language alias analysis from LLVM (used by the alias wrapper).
* **Sea-DSA** – Context-sensitive and field-sensitive analysis with
  Sea-DSA (``lib/Alias/seadsa``). Does not require Boost.
* **SparrowAA** – Inclusion-based pointer analysis without on-the-fly
  call-graph construction (``lib/Alias/SparrowAA``).
* **FPA** – Function Pointer Analysis toolbox (FLTA, MLTA, MLTADF, KELP) under
  ``lib/Alias/FPA`` for resolving indirect calls.
* **DynAA** – Dynamic checker living in ``tools/dynaa`` that validates static
  alias analyses against runtime traces.
* **OriginAA** – K-callsite-sensitive, origin-sensitive analysis targeting
  thread-creation semantics.

Intermediate Representations
----------------------------

See :doc:`../ir/index` for builder APIs and code snippets.

* **Program Dependence Graph (PDG)** – Captures fine-grained data/control
  dependencies.
* **Static Single Information (SSI)** – Planned extension of SSA to encode
  predicate information.
* **DyckVFG** – Value Flow Graph variant designed for Dyck-based alias
  analyses (``lib/Alias/DyckAA/DyckVFG.cpp``).

Machine Learning Features
------------------------

See :doc:`../ml/index` for ML feature extraction APIs.

* **CanaryML** – Memory-related feature extraction using Sea-DSA for ML applications
  (``lib/ML/``). Provides ``MemoryMLFeaturesPass`` for extracting memory access
  patterns and structural features from call sites, useful for training memory
  safety predictors.

Abstract Interpretation
-----------------------

See :doc:`../verification/clam` for CLAM and :doc:`../verification/symabs-ai`
for higher-level abstractions.

* **CLAM** – Modular AI-driven static analyzer with multiple abstract domains
  (``tools/verifier/clam`` and ``lib/Verification/clam``).
* **SymAbsAI** – Configurable abstract interpretation framework with domain
  composition (``lib/Verification/SymAbsAI`` and ``include/Verification/SymAbsAI``).

Utilities and Reachability
--------------------------

See :doc:`../utils/index` and :doc:`../cfl/index` for extended guides.

* **cJSON** – Lightweight JSON parser (``include/Support/cJSON.h``).
* **Transform** – LLVM bitcode transformation passes housed in ``lib/Transform``.
* **CFL Reachability** – General-purpose CFL reachability utilities and
  tooling (``tools/cfl``)
