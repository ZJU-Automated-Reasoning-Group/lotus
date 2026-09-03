WPDS (Weighted Pushdown Systems)
===============================

Weighted pushdown system solvers used to model interprocedural control flow
with stack-aware summaries.

Overview
--------

The WPDS backend provides algorithms for reasoning about weighted pushdown
systems, which can encode interprocedural programs with call/return structure.

**Location**: ``third-party/WPDS/``

**Main capabilities**:

- Representation of pushdown rules and stacks.
- Extensible weight domains for dataflow information.
- Reachability and summary computation algorithms.

Typical Use Cases
-----------------

- Context-sensitive dataflow analysis with summaries.
- Path-sensitive reasoning over call/return structure.
- Experiments with weighted pushdown-system based formulations.

Basic Usage (C\+\+)
-------------------

.. code-block:: cpp

   #include <WPDS/WPDS.h>

   // Instantiate wpds::WPDS<T> with an analysis-specific semiring weight.

Features
--------

- **Weighted automata** – Weighted transition systems over pushdown rules.
- **Pushdown model** – Stack-based semantics for modeling function calls and
  returns.
- **Path analysis** – Algorithms for computing summary weights and reachability
  information.

Integration Notes
-----------------

The WPDS backend is typically used by higher-level analyses that require
interprocedural reasoning with explicit call stacks. See :doc:`index` for a
high-level overview of where WPDS fits in the solver architecture.
