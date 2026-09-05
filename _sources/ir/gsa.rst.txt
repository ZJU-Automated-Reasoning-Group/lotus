GSA — Gated SSA
=========================================

Overview
========

The **Gated SSA (GSA)** layer builds a read-only, immutable view of the control
flow guarding SSA values at join points.

* **Location**: ``lib/IR/GSA/``, ``include/IR/GSA/``

The analysis itself never mutates LLVM IR. A separate materialization pass can
lower lowerable GSA trees into ``select`` chains and optionally replace PHI
nodes.

Key Features
============

* **Read-only analysis**: ``GateAnalysisPass`` builds immutable per-function
  GSA data without inserting or deleting instructions.
* **Explicit control-flow guards**: Branches, switches, invokes, and opaque
  terminators are represented with ``GuardKind`` metadata.
* **First-class gate trees**: Each tracked PHI maps to a ``GateNode`` whose
  root is a ``GateExpr`` of kind ``Bottom``, ``LeafValue``, ``Select``, or
  ``Switch``.
* **Partial lowering**: ``GsaMaterializationPass`` lowers only gates marked as
  lowerable and leaves the rest of the PHIs untouched.

Core Types
==========

**Gate kinds**:

* ``Gamma`` – Standard join PHI
* ``Mu`` – Loop-header PHI
* ``Eta`` – Loop-exit PHI

**Guard kinds**:

* ``BranchTrue`` / ``BranchFalse``
* ``SwitchCase`` / ``SwitchDefault``
* ``InvokeNormal`` / ``InvokeUnwind``
* ``Opaque`` for unsupported non-boolean control flow

**Bottom semantics**:

``Bottom`` is an internal sentinel for unavailable flow. Analysis results never
expose bottom as an LLVM ``Value *``; the materializer lowers it to ``poison``.

Passes
======

* ``ControlDependenceAnalysisPass`` – Reachability + control dependence with
  explicit tracked/untracked block behavior
* ``GateAnalysisPass`` – Builds immutable GSA data
* ``GsaMaterializationPass`` – Lowers lowerable GSA nodes into LLVM IR

Usage
=====

.. code-block:: cpp

   #include "IR/GSA/GSA.h"

   auto *cda = createControlDependenceAnalysisPass();
   auto *ga = createGateAnalysisPass();
   auto *gm = createGsaMaterializationPass();

   cda->runOnModule(M);
   ga->runOnModule(M);

   gsa::GateAnalysis &analysis = ga->getGateAnalysis(F);
   for (const gsa::GateNode *gate : analysis.gates()) {
     if (!gate->isLowerable())
       continue;
     const gsa::GateExpr *root = gate->getRootExpr();
     (void)root;
   }

   // Optional mutating stage.
   gm->runOnModule(M);

Command-Line Options
====================

* ``-gsa-thinned`` – Build thinned GSA (default: true)
* ``-gsa-replace-phis`` – Only affects ``GsaMaterializationPass``; when true,
  lowerable PHIs are replaced with the materialized values

Behavior Notes
==============

* Only blocks reachable from function entry are tracked.
* ``ControlDependenceAnalysis::getCDBlocks()`` returns an empty list for
  untracked blocks.
* ``GateNode::isLowerable()`` is false for gates that depend on non-scalar
  guards such as exception edges or opaque terminators.
* Switch-arm ordering and ``GateAnalysis::gates()`` order are deterministic.
