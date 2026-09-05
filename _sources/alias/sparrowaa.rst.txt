=====================
Sparrow Pointer Analysis — Inclusion AA
=====================

Overview
========

SparrowAA is an inclusion-based points-to analysis.

* **Location**: ``lib/Alias/InclusionBased/SparrowAA``
* **Algorithm**: Inclusion/subset-based pointer analysis with constraint graph construction and worklist-based solving.
* **Typical Use**: Fast whole-program call-graph and mod/ref precomputation

Analysis Modes
==============

* Flow-insensitive, context-insensitive (CI)
* Flow-insensitive, context-sensitive (1-CFA, 2-CFA)

Constraint Types
================

The analysis builds a constraint graph over program pointers using standard
pointer constraints:

* ``p = &x`` → address-of constraint
* ``p = q`` → copy constraint
* ``p = *q`` → load constraint
* ``*p = q`` → store constraint
* ``p = &x->field`` / GEP → field/offset constraint

After collecting constraints, a worklist-based solver propagates points-to sets
until a fixed point is reached.

Optimizations
=============

The implementation supports several optional optimizations (see the README in
``lib/Alias/InclusionBased/SparrowAA`` for details):

* **HVN / HU** – Hash-based value numbering and Heintze–Ullman style
  equivalence to collapse redundant nodes.
* **HCD / LCD** – Hybrid and lazy cycle detection to identify strongly
  connected components and speed up convergence.

**Optimizations** (optional, disabled by default):
* **HVN / HU** – Hash-based value numbering and Heintze–Ullman style equivalence
* **HCD / LCD** – Hybrid and lazy cycle detection for SCC identification

All optimizations are disabled by default and can be enabled via
tool-specific command-line flags.

Features
========

* Inclusion-based algorithm
* Fast analysis for large codebases
* Supports multiple context sensitivities
* Optional optimizations for improved performance

Adaptive Context Sensitivity via HVN
====================================

Hash-based Value Numbering (HVN) in SparrowAA computes pointer-equivalence
labels (``peLabel``) for every node in the constraint graph as a side
effect of its offline variable-substitution pass. These labels encode
whether different callers pass pointer-equivalent or pointer-distinct
arguments to the same formal parameter.

**Key insight**: If two actual arguments at different call sites for the
same formal parameter have *different* ``peLabel``\ s, the function would
benefit from context sensitivity (cloning per caller keeps the pointer
facts separate). If they share the same ``peLabel``, context sensitivity
would produce redundant clones without improving precision.

This enables a lightweight *adaptive* context-sensitivity strategy:

1. Run HVN (already part of SparrowAA's constraint optimization phase).
2. For each function, inspect the ``peLabel``\ s of its incoming arguments
   across all call sites.
3. If arguments from different callers are pointer-distinct, enable context
   sensitivity for that function; otherwise, keep it context-insensitive.

The approach avoids the uniform k-CFA overhead by targeting context
sensitivity only where it measurably improves precision.

See ``lib/Alias/InclusionBased/SparrowAA/IDEA-ADAPTIVE-CS-VIA-HVN.md``
for the full research proposal with examples and analysis.

**Build/run**: Use ``--enable-hvn`` with the SparrowAA frontend to enable
the HVN optimization pass.

Usage
=====

The engine is wrapped as a reusable AA component and also exposed
via a standalone tool:

.. code-block:: bash

   ./build/bin/lotus-alias-sparrow-aa example.bc

In integrated settings (e.g., Clam or LotusAA), it can be selected through
the corresponding configuration files or command-line switches.

Note: this module have some redundancies with aserpta, and reuses some header files from it (from context abstraction).
