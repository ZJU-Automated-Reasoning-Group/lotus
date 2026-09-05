Machine Learning Features (CanaryML)
 ====================================

This section documents the machine learning feature extraction capabilities in Lotus,
specifically the ``CanaryML`` library for memory-related feature extraction using Sea-DSA.

Overview
--------

The ``CanaryML`` library (built from ``lib/Analysis/FeatureExtraction/``) provides utilities for extracting
memory-related features from program call sites. These features are designed for
machine learning applications, particularly for predicting memory safety properties
or learning memory access patterns.

**Location**: ``lib/Analysis/FeatureExtraction/``

**Library Name**: ``CanaryML``

**Dependencies**:
- Sea-DSA for points-to graph construction
- LLVM analysis passes (CallGraph, TargetLibraryInfo, AllocWrapInfo)

MemoryMLFeaturesPass
---------------------

The primary component is ``MemoryMLFeaturesPass``, an LLVM ModulePass that extracts
features for each call site in a module.

**LLVM Pass Name**: ``-Pmem-ml-features``

**Header**:
.. code-block:: cpp

   #include "Analysis/FeatureExtraction/MemoryMLFeatures.h"

**Basic Usage**:
.. code-block:: cpp

   #include "Analysis/FeatureExtraction/MemoryMLFeatures.h"

   // Create and run the pass
   previrt::MemoryMLFeaturesPass ml_features_pass;
   ml_features_pass.runOnModule(M);

   // Extract features for a specific call site
   for (auto &F : M) {
       for (auto &B : F) {
           for (auto &I : B) {
               if (auto *CB = dyn_cast<llvm::CallBase>(&I)) {
                   auto features = ml_features_pass.extractMLFeatures(*CB);
                   features.write(errs());
               }
           }
       }
    }

**Command-Line Options**:

* ``-Pinclude-expensive-ml-features`` (default: true)
  Include expensive-to-compute memory access features.

Features Extracted
-----------------

The pass extracts two sets of features for each call site:

1. **Callee Summary Graph Features** (prefix ``callee_``)
2. **Top-Down Unification Features** (prefix ``td_callee_``)

**Available Feature Accessors**:

Graph Structure:
- ``getCalleeNumNodes()`` / ``getTdCalleeNumNodes()``
- ``getCalleeNumAccessed()`` / ``getTdCalleeNumAccessed()``
- ``getCalleeNumCollapsed()`` / ``getTdCalleeNumCollapsed()``
- ``getCalleeNumSequence()`` / ``getTdCalleeNumSequence()``

Memory Allocation:
- ``getCalleeNumAlloca()`` / ``getTdCalleeNumAlloca()`` (stack)
- ``getCalleeNumHeap()`` / ``getTdCalleeNumHeap()`` (heap)
- ``getCalleeNumExternal()`` / ``getTdCalleeNumExternal()`` (external)
- ``getCalleeNumAllocSites()`` / ``getTdCalleeNumAllocSites()``

Pointers and Accesses:
- ``getCalleeNumPointers()`` / ``getTdCalleeNumPointers()``
- ``getCalleePointersPerNode()`` / ``getTdCalleePointersPerNode()``

Feature Output
--------------

Features can be written to a stream:

.. code-block:: cpp

   features.write(llvm::errs());

Output format::

   ## ML features for call @function_name
   Callee's Summary graph
     Number of nodes          : <N>
     Number of collapsed nodes: <N>
     Number of accessed nodes : <N>
      ...

Analysis Dependencies
--------------------

The pass requires the following LLVM analyses:

- ``TargetLibraryInfoWrapperPass``
- ``CallGraphWrapperPass``
- ``AllocWrapInfo``
- ``DsaLibFuncInfo``

Integration Notes
----------------

The ``MemoryMLFeaturesPass`` is part of the ``previrt`` namespace and integrates
Sea-DSA for memory modeling. It is particularly useful for:

1. Building datasets for memory safety prediction
2. Learning patterns in library function effects
3. Analyzing memory access characteristics of call sites
4. Generating features for ML-based bug detection

Related Components
------------------

- See :doc:`../alias/seadsa` for Sea-DSA documentation
- See :doc:`../analysis/index` for related analysis passes
