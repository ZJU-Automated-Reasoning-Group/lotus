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

Abstract Domain Contract
========================

The solver is generic over an abstract domain instead of taking handwritten
lattice operations from the analysis ``Problem``. Domains are declared in
``include/Dataflow/Mono/Core/AbstractDomain.h`` and must satisfy a small
formal contract, detected by the ``IsMonoAbstractDomain`` trait:

* ``value_type``: the type of facts the domain manipulates,
* ``bottom()``: the least element of the lattice,
* ``join(const value_type &Lhs, const value_type &Rhs)``: the lattice join,
* ``equal(const value_type &Lhs, const value_type &Rhs)``: fact equality.

The header ships three ready-made domains. ``UnionDomain<ContainerT>`` is the
may-style domain used by the bit-vector clients: ``bottom()`` is the empty
container and ``join`` inserts every element of ``Rhs`` into ``Lhs``.
``IntersectionDomain<ContainerT>`` is the must-style counterpart: it is
constructed with a ``Universe`` (also settable via ``setUniverse``),
``bottom()`` returns the ``Universe``, and ``join`` keeps only elements present
in both operands. ``LegacyProblemDomain<ValueT>`` provides a default contract
for legacy problems and is flagged with ``is_legacy = true``. All three also
provide a ``widen`` operation.

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

