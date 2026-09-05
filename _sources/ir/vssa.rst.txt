vSSA — Variable Static Single Assignment
========================================

Overview
========

**vSSA (Variable Static Single Assignment)** is a variant of SSI (Static Single
Information) designed specifically for enabling precise symbolic range analysis.
It transforms SSA-form IR by introducing sigma (σ) and phi (φ) functions at
control-flow splits and joins, providing fine-grained version information for
variables involved in comparison operations.

* **Location**: ``lib/IR/vSSA/``, ``include/IR/vSSA/``

vSSA is used by the Strict Relations Alias Analysis (SRAA) to perform symbolic
range analysis for pointer disambiguation. By splitting live ranges at critical
program points, vSSA enables the range analysis to maintain precise bounds for
variables along different execution paths.

Key Features
============

* **Live-Range Splitting at Comparisons**: Inserts σ-functions for operands of
  comparison instructions (ICmp, Switch) at control-flow splits.
* **Dominance Frontier Processing**: Uses dominance frontiers to determine where
  additional φ-functions are needed to maintain SSA properties.
* **Condition-Sensitive Reasoning**: Enables analyses to reason about variable
  values in the context of branch conditions.
* **Lazy Dominance Frontier Computation**: Computes dominance frontiers on demand
  rather than pre-computing for all blocks.

vSSA Instructions
=================

The vSSA transformation introduces two types of special instructions:

**vSSA Sigma (σ) Functions**:

* Named with prefix ``vSSA_sigma``
* Inserted at successor blocks of conditional branches and switches
* Create separate variable versions for each outgoing path
* Applied to operands of comparison instructions (ICmp, Switch conditions)

**vSSA Phi (φ) Functions**:

* Named with prefix ``vSSA_phi``
* Inserted at dominance frontiers of blocks containing σ-functions
* Merge values from different control-flow paths
* Maintain SSA properties after live-range splitting

Algorithm
=========

The vSSA transformation algorithm proceeds in several stages:

1. **Unreachable Block Removal**:

   Remove unreachable basic blocks from the function, as the DominatorTree pass
   treats unreachable blocks as dominated by anything, which can break the
   transformation algorithm.

2. **Sigma Function Insertion**:

   For each basic block with a conditional terminator:

   * **Branch Instructions**: For conditional branches with ICmp conditions,
     create σ-functions for each operand of the comparison. Also handle
     indirect operands through CastInst (ZExt, SExt, Trunc).

   * **Switch Instructions**: Create σ-functions for the switch condition
     value, handling cast instructions similarly.

   σ-functions are only inserted when:

   * The successor block has a single predecessor
   * The predecessor dominates or has the successor in its dominance frontier
   * An identical σ-function does not already exist

3. **Phi Function Insertion**:

   For each σ-function created, compute the dominance frontier and insert
   φ-functions where needed:

   * The φ-functions are inserted in blocks where the original value dominates
     the block and the block dominates some use of the value.

4. **Variable Renaming**:

   Rename uses of variables to reference their new versions:

   * Uses in the dominator tree of a σ/φ-function are renamed to use the new
     version.
   * Uses in the dominance frontier are renamed selectively based on incoming
     edges.

5. **Recursive Processing**:

   The insertion of φ-functions may require additional φ-functions to be
   inserted, so the algorithm processes newly created φ-functions recursively.

Components
==========

**vSSA Pass** (``vSSA.h``, ``vSSA.cpp``):

The main transformation pass that converts SSA to vSSA form:

* ``runOnFunction()`` – Main entry point that processes a function
* ``createSigmasIfNeeded()`` – Examines terminator instructions to determine
  where σ-functions are needed
* ``insertSigmas()`` – Creates σ-functions for a value at all appropriate
  successors
* ``insertPhisForSigma()`` – Creates φ-functions in dominance frontiers
* ``renameUsesToSigma()`` / ``renameUsesToPhi()`` – Rename variable uses to
  reference new versions
* ``populatePhis()`` – Fill in missing operands of φ-functions
* ``computeDominanceFrontier()`` – Compute dominance frontier on demand

Usage
=====

**As an LLVM Pass**:

.. code-block:: cpp

   #include "IR/vSSA/vSSA.h"
   
   // vSSA requires DominatorTree
   void getAnalysisUsage(AnalysisUsage &AU) const {
       AU.addRequired<DominatorTreeWrapperPass>();
   }
   
   // Run the pass
   vSSA *VSSAPass = new vSSA();
   VSSAPass->runOnFunction(F);

**Command-Line Usage**:

.. code-block:: bash

   opt -load libCanaryvSSA.so -vssa < input.bc > output.bc

**Checking for vSSA Instructions**:

.. code-block:: cpp

   #include "llvm/IR/Instructions.h"
   
   Instruction *I = ...;
   PHINode *PN = dyn_cast<PHINode>(I);
   
   if (PN && PN->getName().startswith("vSSA_sigma")) {
       // I is a vSSA sigma function
   }
   
   if (PN && PN->getName().startswith("vSSA_phi")) {
       // I is a vSSA phi function
   }

Integration with SRAA
=====================

vSSA is designed to work with the Strict Relations Alias Analysis (SRAA):

1. **Range Analysis**: The range analysis component identifies σ-functions by
   their name prefix (``vSSA_sigma``) and uses them to establish constraints
   on variable ranges.

2. **Symbolic Expressions**: vSSA's live-range splitting enables the range
   analysis to maintain separate symbolic expressions for each variable version.

3. **Pointer Disambiguation**: By providing precise range information, vSSA
   helps SRAA prove that pointers cannot alias based on their computed ranges.

Example
=======

Consider the following code:

.. code-block:: c

   int foo(int x, int y) {
       if (x > y) {
           return x - y;
       } else {
           return y - x;
       }
   }

After vSSA transformation, the IR will contain σ-functions for ``x`` and ``y``
at the branch split:

.. code-block:: llvm

   define i32 @foo(i32 %x, i32 %y) {
   entry:
     %cmp = icmp sgt i32 %x, %y
     br i1 %cmp, label %if.then, label %if.else
   if.then:
     %x.sigma = phi i32 [ %x, %entry ]  ; vSSA_sigma
     %y.sigma = phi i32 [ %y, %entry ]  ; vSSA_sigma
     %sub = sub i32 %x.sigma, %y.sigma
     ret i32 %sub
   if.else:
     %x.sigma.1 = phi i32 [ %x, %entry ]  ; vSSA_sigma
     %y.sigma.1 = phi i32 [ %y, %entry ]  ; vSSA_sigma
     %sub.1 = sub i32 %y.sigma.1, %x.sigma.1
     ret i32 %sub.1
   }

This enables the range analysis to reason separately about the values of ``x``
and ``y`` in each branch.

References
==========

* Victor Hugo Sperle Campos. *Range Analysis and its Application to the
  Disambiguation of Pointers in C Programs*. Master's thesis, Federal
  University of Minas Gerais, 2013.
