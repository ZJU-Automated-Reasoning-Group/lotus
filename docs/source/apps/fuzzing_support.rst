Fuzzing Support
================

Lotus provides directed greybox fuzzing techniques and profiling infrastructure for test generation.

Overview
--------

Integrates AFLGo, Hawkeye, and DAFL algorithms for directed greybox fuzzing. The implementation is based on libaflgo and provides modular distance analysis and instrumentation.

**Location**: ``lib/Fuzzing/``

**Components**: Directed fuzzing algorithms, distance analysis, target detection, compiler/linker plugins, IR mutation utilities.

**Algorithms Supported**:
* **AFLGo (CCS 17)**: Distance-based power scheduling, call graph distances
* **Hawkeye (CCS 18)**: Function-level analysis, rare branch prioritization
* **DAFL (USENIX Security 23)**: Data dependency guided, taint integration

Directed Greybox Fuzzing Algorithms
------------------------------------

**AFLGo (CCS 17)**: Distance-based power scheduling using call graph distances.

**Hawkeye (CCS 18)**: Function-level analysis with rare branch prioritization.

**DAFL (USENIX Security 23)**: Data dependency guided fuzzing with taint integration.

Distance Analysis
-----------------

The ``Analysis/`` directory provides distance computation for directed fuzzing:

* **BasicBlockDistance.cpp** – AFLGo basic block distance analysis
  * Computes distances from basic blocks to target functions
  * Uses call graph analysis for distance calculation

* **FunctionDistance.cpp** – Hawkeye function distance analysis
  * Function-level distance computation
  * Rare branch prioritization support

* **DAFL.cpp** – DAFL data-flow distance analysis
  * Data dependency guided distance computation
  * Memory tracking and taint integration

* **ExtendedCallGraphAnalysis.cpp** – Enhanced call graph with pointer analysis
  * Extends standard call graph with pointer analysis information
  * Improves accuracy of distance calculations

* **TargetDetection.cpp** – Target identification for instrumentation
  * Automatic target detection from program structure
  * Supports manual target specification

**Usage**:
.. code-block:: cpp

   #include <Fuzzing/Analysis/BasicBlockDistance.h>
   auto distance = BasicBlockDistance(module);
   auto distances = distance.computeDistances(targetFunctions);

Compiler and Linker Plugins
----------------------------

**AFLGoCompiler/** – LLVM compiler plugin for compile-time instrumentation:

* ``Plugin.cpp`` – LLVM plugin entry point for AFLGo instrumentation
* ``TargetInjection.cpp`` – Target injection for directed fuzzing

**AFLGoLinker/** – LLVM linker plugin for link-time instrumentation:

* ``Plugin.cpp`` – Link-time instrumentation plugin
* ``DAFL.cpp`` – DAFL-specific instrumentation
* ``DistanceInstrumentation.cpp`` – Distance-based instrumentation
* ``FunctionDistanceInstrumentation.cpp`` – Function-level distance instrumentation
* ``DuplicateTargetRemoval.cpp`` – Target deduplication
* ``TargetInjectionFixup.cpp`` – Target injection fixup

IR Mutation
-----------

**IRMutate/** – LLVM IR mutation utilities:

* ``llvm_mutate.cpp`` – LLVM IR mutation passes

Target Detection
----------------

Automatic target identification and instrumentation support.

**Usage**:
.. code-block:: cpp

   #include <Fuzzing/Analysis/TargetDetection.h>
   auto detector = TargetDetection(module);
   auto targets = detector.findTargets();
