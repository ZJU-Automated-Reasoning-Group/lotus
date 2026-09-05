Symbolic Execution
==================

This section documents the top-level symbolic execution engine that backs the
``lotus-check --engine=symex`` checker and other path-sensitive analyses.

**Headers**: ``include/SymbolicExecution/``

**Implementation**: ``lib/SymbolicExecution/``

**Build target**: ``CanarySymbolicExecution``

**Tool frontend**: ``lotus-check --engine=symex`` (see :doc:`../checker/symex`)

Overview
--------

The ``SymbolicExecution`` subsystem is a standalone, path-sensitive symbolic
execution engine. It operates over the guarded value-flow graph (GVFG), tracks
symbolic scalar values and memory facts along feasible paths, and uses SMT-backed
path-condition checks before emitting bug reports.

The subsystem lives at the top level of the source tree (``lib/SymbolicExecution/``
and ``include/SymbolicExecution/``) rather than under ``lib/Analysis/`` because it
owns a full driver, symbolic state model, solver bridge, memory modeling layer,
taint model, and LLVM pass wrapper. It is consumed by the checker framework
through ``SymbolicExecutionWrapper``.

Core Components
---------------

The engine is composed of the following headers under
``include/SymbolicExecution/``:

Driver and state
~~~~~~~~~~~~~~~~

* ``AnalysisDriver`` – Coordinates whole-module symbolic execution and summary
  scheduling across functions.
* ``AnalysisState`` – Represents symbolic memory, guarded values, points-to
  items, interprocedural summaries, taint updates, and pending bug queries.
* ``AnalysisLimit`` – Configurable resource and depth budgets that bound
  exploration.

Symbolic values and properties
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

* ``ProgramVar`` – Names symbolic values tied to LLVM values and contexts.
* ``PropertyAllocator`` – Allocates and manages symbolic properties.
* ``PropertyValue``, ``PropertyInteger``, ``PropertySym`` – Represent scalar
  properties such as offsets, sizes, and affine expressions attached to
  symbolic values.
* ``BigInteger`` – Arbitrary-precision integer support for symbolic reasoning.

Path conditions and solving
~~~~~~~~~~~~~~~~~~~~~~~~~~~

* ``ConstraintRepr`` – Encodes symbolic predicates used in path conditions.
* ``PathCondSolver`` – Discharges path-feasibility queries through the SMT
  (Z3) backend; paths proven infeasible are pruned before reporting.

Memory and control flow
~~~~~~~~~~~~~~~~~~~~~~~~

* ``MemoryAPI`` – Connects symbolic execution to allocation modeling, data
  layout, and library function summaries.
* ``GVFGUtility`` – Builds and queries the guarded value-flow graph that the
  engine walks.
* ``InstResolver`` – Resolves instruction-level targets (call targets, memory
  locations) during exploration.
* ``DomTreePass`` – Dominator-tree reasoning used for reachability and join
  handling.

Taint and reporting
~~~~~~~~~~~~~~~~~~~

* ``TaintModel`` – Provides source, transfer, and sink facts used by
  taint-sensitive bug checks.
* ``SymbolicExecutionWrapper`` – Integrates the engine with the LLVM pass
  pipeline and the checker report infrastructure (``BugReportMgr``).

Analysis flow
-------------

A typical symbolic-execution run proceeds as follows:

1. **GVFG construction** – ``GVFGUtility`` builds the guarded value-flow graph,
   capturing data and guarded control dependencies.
2. **Driver scheduling** – ``AnalysisDriver`` selects entry functions and
   schedules interprocedural exploration, applying summaries where available.
3. **Path exploration** – ``AnalysisState`` advances along paths, updating
   symbolic memory, properties, and taint facts.
4. **Feasibility check** – ``PathCondSolver`` queries the SMT solver; paths
   whose path conditions are unsatisfiable are pruned.
5. **Bug query** – At suspect instructions (e.g. dereferences, divisions), the
   engine emits bug queries through ``AnalysisState``.
6. **Reporting** – Confirmed bugs flow through ``SymbolicExecutionWrapper`` into
   the centralized ``BugReportMgr`` (JSON/SARIF output).

Usage
-----

The engine is invoked through the unified checker frontend:

.. code-block:: bash

   ./build/bin/lotus-check --engine=symex input.bc
   ./build/bin/lotus-check --engine=symex input.bc --checks=null-deref,use-after-free

For the full list of engine options, see :doc:`../checker/symex`.

Programmatic usage
------------------

The engine is exposed as an LLVM pass via ``SymbolicExecutionWrapper`` and can be
added to a pass pipeline like other Lotus passes:

.. code-block:: cpp

   #include "SymbolicExecution/SymbolicExecutionWrapper.h"

   // Within a pass manager setup:
   auto *seWrapper = new SymbolicExecutionWrapperPass();
   pm.add(seWrapper);

Tests
-----

Focused unit tests live under ``tests/unit/SymbolicExecution`` and build against
``CanarySymbolicExecution``. They cover spec compatibility and the taint model.

See Also
--------

* :doc:`../checker/symex` – ``lotus-check --engine=symex`` checker frontend
* :doc:`../ir/gvfg` – Guarded Value-Flow Graph (GVFG) IR
* :doc:`../solvers/smt` – SMT (Z3) solver backend
* :doc:`../user_guide/architecture` – Architecture overview
