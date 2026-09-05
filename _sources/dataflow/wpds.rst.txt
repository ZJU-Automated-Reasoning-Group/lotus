WPDS Dataflow Engine
====================

Overview
========

The **WPDS engine** in ``lib/Dataflow/WPDS`` solves distributive
interprocedural data-flow problems by encoding them as
**Weighted Pushdown Systems (WPDS)**.
It leverages the WALi-style library (``third-party/WPDS``) to compute
post*/pre* reachability over a pushdown system that models both
control-flow and call/return behavior.

* **Location**: ``lib/Dataflow/WPDS``
* **Core classes**: ``InterProceduralDataFlowEngine``,
  ``GenKillTransformer``, ``DataFlowFacts``

Core Idea
=========

Instead of building an exploded super-graph (as IFDS/IDE do), the WPDS
engine:

* constructs a pushdown system whose rules correspond to CFG edges and
  call/return transitions,
* attaches **weights** that model GEN/KILL behavior on a set of facts,
* runs a standard WPDS algorithm (post* or pre*) to compute the least
  fixed point.

Clients typically provide a function
``GenKillTransformer *createTransformer(Instruction *I)`` which
captures the effect of each instruction on the fact set.

Example Analyses
================

Constant Propagation
--------------------

``WPDSConstantPropagation`` demonstrates a forward **constant
propagation** analysis:

* facts track variables that currently hold constant values,
* GEN adds variables whose values are known to be constant at a point,
* KILL removes variables whose values become non-constant,
* the interprocedural engine propagates these facts across calls.

Liveness
--------

``WPDSLivenessAnalysis`` performs a backward **liveness analysis**:

* GEN collects variables used at an instruction (including loads and
  uses in stores),
* KILL records variables defined (or overwritten) at that point,
* the WPDS formulation makes the analysis naturally interprocedural.

Taint Analysis
--------------

``WPDSTaintAnalysis`` is a WPDS-based **taint analysis**:

* facts represent tainted values,
* GEN introduces taint at configurable source calls,
* KILL removes taint at sanitizers,
* the engine reports flows of tainted data into dangerous sinks
  (e.g., ``system``, ``exec``, ``strcpy``).

Uninitialized Variables
-----------------------

``WPDSUninitializedVariables`` shows a forward analysis for detecting
reads from **possibly uninitialized memory**:

* GEN marks newly allocated locals (``alloca``) as uninitialized,
* KILL removes a location when it is definitely initialized by a store,
* any load whose pointer appears in the ``IN`` set is reported as a
  potential use of uninitialized data.

Compared to IFDS/IDE, the explicit pushdown-system encoding makes the
call stack and recursion behavior more visible and can be advantageous
for certain classes of interprocedural problems.

.. note::

   **WALi-OpenNWA** (``third-party/WALi-OpenNWA/``) is a modern C++
   reimplementation of the WPDS library. It provides an alternative WPDS
   backend with modern C++ features. The existing
   ``lib/Dataflow/WPDS/`` implementation (backed by
   ``third-party/WPDS/``) continues to be the default. Build with
   ``-DLOTUS_ENABLE_WALI_OPENNWA=ON`` to enable the WALi-OpenNWA
   backend.

