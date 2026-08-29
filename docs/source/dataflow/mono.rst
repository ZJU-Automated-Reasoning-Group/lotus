Monotone Dataflow Engine
========================

Overview
========

The **monotone dataflow engine** in ``lib/Dataflow/Mono`` implements a
classic **bit-vector style** framework for intraprocedural and
interprocedural analyses over LLVM IR.

* **Headers**: ``include/Dataflow/Mono``
* **Compiled analyses**: ``lib/Dataflow/Mono``
* **Main classes**: ``IntraMonoSolver``, ``InterMonoSolver``
* **Direction**: forward or backward (configurable per analysis)

Implementation Layout
=====================

* ``Core/`` contains generic call-string context representation.
* ``Solver/`` contains the intra/inter solvers and call-string engine.
* ``LLVM/`` contains LLVM problem interfaces and solver-facing analysis types.
* ``Domains/`` contains named abstract fact domains.
* ``Analyses/Intra/`` and ``Analyses/Inter/`` contain concrete clients.
* ``Container/`` and ``Support/`` provide reusable fact containers, results,
  diagnostics, and soundness metadata.

Core Idea
=========

Facts are represented as sets of LLVM values (``std::set<llvm::Value*>``).
For each instruction ``n`` an analysis defines:

* ``GEN[n]`` — facts generated at ``n``,
* ``KILL[n]`` — facts killed at ``n``,
* ``IN[n]`` — facts before executing ``n``,
* ``OUT[n]`` — facts after executing ``n``.

The solver repeatedly applies client-provided ``normalFlow`` and ``merge``
operations until all ``IN``/``OUT`` facts reach a monotone fixed point.

Example Analyses
================

Live Variables (SSA)
--------------------

``runLiveVariablesAnalysis`` implements a **backward liveness analysis**
for SSA registers:

* **Direction**: backward.
* **Facts**: SSA values that are live at a program point.
* **Equations**:

  * ``GEN[n]`` = operands of ``n`` that are instructions or arguments,
  * ``KILL[n]`` = ``{n}`` if ``n`` defines a non-void value,
  * ``OUT[n]`` = ⋃ ``IN[s]`` for all CFG successors ``s``,
  * ``IN[n]`` = ``(OUT[n] - KILL[n]) ∪ GEN[n]``.

Reachable Instructions
----------------------

``runReachableAnalysis`` is another client that computes which
instructions are **reachable in the future**:

* **Direction**: backward.
* **Facts**: instructions that can be executed after ``n``.
* **Equations**:

  * ``GEN[n]`` = ``{n}`` if a user-supplied predicate ``filter(n)`` holds,
  * ``KILL[n]`` = ∅,
  * ``OUT[n]`` = ⋃ ``IN[s]`` for all successors ``s``,
  * ``IN[n]`` = ``GEN[n] ∪ OUT[n]``.

Both examples show how to express standard gen–kill problems while
delegating the fixed-point iteration to ``IntraMonoSolver``.

