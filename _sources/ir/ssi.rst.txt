SSI — Static Single Information
================================

Overview
========

**Static Single Information (SSI)** is an extension of SSA (Static Single
Assignment) that augments variables with additional predicate and path
information. While SSA ensures that every definition of a variable dominates
all its uses, SSI additionally guarantees that **every use of a variable
post-dominates all its reaching definitions**. This dual property enables more
precise path-sensitive and condition-sensitive analyses.

* **Location**: ``lib/IR/SSI/``, ``include/IR/SSI/``

The SSI transformation converts SSA-form IR into SSI form by introducing
**sigma (σ) functions** at control-flow splits, analogous to how SSA introduces
phi (φ) functions at control-flow joins. This symmetric treatment of forward
(domination) and backward (post-domination) properties makes SSI particularly
useful for analyses that need to reason about value relationships across
conditional branches.

Key Features
============

* **Dual Dominance Property**: Extends SSA's dominance property with
  post-dominance, ensuring that uses post-dominate their reaching definitions.
* **Sigma Functions**: Introduces σ-functions at control-flow splits to encode
  predicate information and value versions along different paths.
* **Path-Sensitive Analysis**: Provides explicit representation of values along
  different control-flow paths, enabling more precise analysis.
* **Condition-Sensitive Reasoning**: Encodes relationships between values
  guarded by conditions, making it easier to reason about conditional value
  relationships.
* **Live-Range Splitting**: Splits variable live ranges at strategic program
  points to maintain SSI properties.

SSI Formalism
=============

SSI form is constructed as follows:

1. **Start from SSA form**: The transformation assumes input IR is already in
   SSA form (with φ-functions at joins).

2. **Compute iterated post-dominance frontier**: Similar to how SSA uses the
   dominance frontier to determine where φ-functions are needed, SSI uses the
   iterated post-dominance frontier to determine where σ-functions are needed.

3. **Insert σ-functions**: At each control-flow split whose successors are not
   in the same post-domination tree region, insert σ-functions that create new
   variable versions for each outgoing path.

4. **Rename variables**: Perform a renaming pass to assign unique names to σ
   results, mirroring the SSA renaming process.

The transformation maintains two key invariants:

* Every definition of a variable dominates all its uses (SSA property).
* Every use of a variable post-dominates all its reaching definitions (SSI
  property).

Components
==========

**SSIfy** (``SSI.h``, ``SSIPass.cpp``, ``SSITransform.cpp``):

The main transformation pass that converts SSA to SSI form:

* ``runOnFunction()`` – Main entry point that processes a function
* ``run()`` – Determines the splitting strategy for a variable and invokes
  split and rename operations
* ``split()`` – Splits live ranges of variables at strategic program points
* ``rename_initial()`` and ``rename()`` – Rename variables to maintain SSI
  invariants after splitting
* ``clean()`` – Removes unnecessary SSI nodes (σ-functions, φ-functions, copies)

**ProgramPoint** (``SSIUtils.cpp``):

Represents a location in the program where SSI transformations may occur:

* **In**: Entry point of a block (join point, φ insertion)
* **Self**: Middle of a block (parallel copy insertion)
* **Out**: Exit point of a block (branch point, σ insertion)

Program points are identified by an instruction and a position, and are used to
determine where σ-functions, φ-functions, and copy instructions should be
inserted.

**RenamingStack** (``SSIUtils.cpp``):

Manages the stack of variable definitions during renaming. Maintains the
current version of a variable for each basic block visited during the
topological traversal.

**PostDominanceFrontier** (``SSI.h``):

Computes the post-dominance frontier for basic blocks, which is used to
determine where σ-functions must be inserted. The post-dominance frontier is
analogous to the dominance frontier used in SSA construction, but operates in
reverse control flow.

SSI Instructions
================

The SSI transformation introduces three types of special instructions:

**SSI Phi (φ) Functions**:

* Named with prefix ``SSIfy_phi``
* Similar to SSA φ-functions, used at join points to merge values from
  different control-flow paths
* May be inserted at dominance frontiers to maintain SSI properties

**SSI Sigma (σ) Functions**:

* Named with prefix ``SSIfy_sigma``
* Inserted at control-flow splits (branch points) to create separate variable
  versions for each outgoing path
* A σ-function takes one input value and produces a versioned output for each
  successor block of the branch
* These functions encode predicate information by associating values with the
  conditions that guard their paths

**SSI Copy Instructions**:

* Named with prefix ``SSIfy_copy``
* Identity operations (add with zero) inserted at use points to split live
  ranges
* Used when fine-grained splitting is needed at specific instruction locations

Algorithm
=========

The SSI transformation algorithm proceeds in several stages:

1. **Program Point Identification**:

   For each variable, identify initial program points where splitting may be
   needed:

   * **Conditional exits** (downwards/upwards): Points where variables are
     used in branch conditions
   * **Uses** (downwards/upwards): Points where variables are used in
     instructions

2. **Live Range Splitting**:

   The ``split()`` function computes two sets of program points:

   * **Sup**: Points derived from upward program points using iterated
     post-dominance frontiers
   * **Sdown**: Points derived from downward program points using iterated
     dominance frontiers

   At each identified point, insert the appropriate SSI instruction:
   * σ-functions at branch points (Out positions)
   * φ-functions at join points (In positions)
   * Copy instructions at use points (Self positions)

3. **Variable Renaming**:

   After splitting, rename all variable versions to maintain SSI invariants:

   * Traverse the CFG in topological order (respecting dominator tree)
   * Maintain a renaming stack for each variable
   * Update uses to reference the most recent definition that dominates them
   * Handle σ-functions and φ-functions during renaming

4. **Cleanup**:

   Remove unnecessary SSI instructions:

   * Remove σ-functions, φ-functions, and copies that don't contribute to the
     final SSI form
   * Use topological sorting to safely remove instructions in dependency order

Usage
=====

**As an LLVM Pass**:

.. code-block:: cpp

   #include "IR/SSI/SSI.h"
   
   // SSI transformation is typically run as part of a pass pipeline
   // The pass requires DominatorTree, PostDominatorTree, and DominanceFrontier
   FunctionPass *createSSIfyPass();

**Command-Line Options**:

The SSI transformation accepts several command-line options:

* ``-v`` – Enable verbose mode, printing detailed information about
  transformations
* ``-set xxxx`` – Configure initial program points (each x is either 0 or 1):
  
  * **1st bit**: Exit of conditionals, downwards
  * **2nd bit**: Exit of conditionals, upwards
  * **3rd bit**: Uses, downwards
  * **4th bit**: Uses, upwards

  For example, ``-set 1100`` enables splitting at conditional exits (both
  directions) but not at uses.

**Checking for SSI Instructions**:

.. code-block:: cpp

   #include "IR/SSI/SSI.h"
   
   Instruction *I = ...;
   
   if (SSIfy::is_SSIphi(I)) {
       // I is an SSI phi function
   }
   
   if (SSIfy::is_SSIsigma(I)) {
       // I is an SSI sigma function
   }
   
   if (SSIfy::is_SSIcopy(I)) {
       // I is an SSI copy instruction
   }
   
   if (SSIfy::is_actual(I)) {
       // I is not an SSI-created instruction (actual program instruction)
   }

Integration
===========

SSI is used by several analyses and transformations in Lotus:

* **Strict Relations Alias Analysis (SRAA)**: Uses a variant of SSI called
  **vSSA** (variable SSA) to enable precise symbolic range analysis for
  pointer disambiguation. The vSSA transformation splits live ranges at
  control-flow joins, forks, and uses of pointer-index expressions, providing
  fine-grained version information for symbolic analysis.

* **Path-Sensitive Analyses**: Analyses that need to reason about values along
  different control-flow paths benefit from SSI's explicit encoding of
  predicate and path information.

* **Range Analysis**: Symbolic range analysis uses SSI/vSSA to maintain
  precise bounds for variables along different execution paths, enabling
  stronger constraints for proving pointer disjointness.

* **Constraint-Based Analyses**: Analyses that construct constraint systems
  benefit from SSI's ability to associate values with the conditions that
  guard them, allowing for more precise constraint generation.

The SSI transformation provides a foundation for analyses that require
path-sensitive and condition-sensitive reasoning, making it easier to prove
properties about programs with complex control flow and conditional
dependencies.

