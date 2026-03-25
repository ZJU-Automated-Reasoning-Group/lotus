IFDS / IDE Engine
=================

Overview
========

The **IFDS / IDE engine** in ``lib/Dataflow/IFDS`` implements the
algorithms of Reps, Horwitz, and Sagiv for:

* **IFDS** — Interprocedural Finite Distributive Subset problems,
* **IDE**  — Interprocedural Distributive Environment problems.

Both are **context-sensitive**, flow-sensitive frameworks expressed
over an **exploded super-graph** whose nodes are pairs
``(program point, data-flow fact)``.

* **Location**: ``lib/Dataflow/IFDS``
* **Solvers**: ``IFDSSolver``, ``ParallelIFDSSolver``, ``IDESolver``

IFDS: Set-Valued Problems
=========================

IFDS targets analyses where facts are just **elements of a finite set**
and the merge operator is set union (may-analysis).
Each analysis defines:

* a finite domain ``D`` of path facts and a distinguished zero fact,
* flow functions for:

  * normal instructions,
  * calls,
  * returns,
  * call-to-return (summary edges).

Example — Taint Analysis
------------------------

``ifds::TaintAnalysis`` is an IFDS client that tracks **tainted
variables** and **tainted memory locations**:

* facts distinguish tainted SSA values vs. tainted memory,
* normal flow propagates taint through loads, stores, arithmetic, casts,
  and GEPs,
* call / return flow map taint across function boundaries,
* call-to-return flow models configurable **sources**, **sinks**, and
  **sanitizers** using the taint configuration files in ``config/``.

IDE: Value-Enriched Problems
============================

IDE extends IFDS by attaching a **value** to each fact, forming an
environment ``D → L`` where ``L`` is a lattice.
Edge functions describe how these values are transformed along the
exploded super-graph.

IDE is useful for:

* constant propagation,
* typestate tracking,
* must-analyses where join is not simple union.

Example — Linear Constant Propagation
-------------------------------------

``IDEConstantPropagation`` associates integer-typed facts with a small
lattice:

* ``⊥`` — unreachable,
* ``k`` — known constant value,
* ``⊤`` — unknown / non-constant.

Edge functions interpret assignments, copies, and arithmetic
instructions so that, whenever both operands are constant, a constant
result is propagated; otherwise, the lattice moves toward ``⊤``.

Example — Typestate Analysis
----------------------------

``IDETypeState`` tracks **finite-state properties** such as:

* file handles (``fopen``/``fclose``),
* locks (``pthread_mutex_lock`` / ``unlock``),
* heap objects (``malloc``/``free``),
* sockets and protocol states.

Clients describe a typestate property by:

* defining named states (including error states),
* registering transitions for operations (functions, opcodes, or custom
  predicates).

Edge functions then implement these transitions over the typestate
lattice, and the IDE solver yields, at each program point, the
abstract state of tracked objects.

Usage
=====

At a high level, an IFDS/IDE analysis is instantiated and solved as:

.. code-block:: cpp

   // Pseudo-code sketch
   using Analysis = ifds::TaintAnalysis; // or IDEConstantPropagation, IDETypeState, ...
   Analysis problem;
   IFDSSolver<Analysis> solver(problem);
   solver.solve();

Command-Line Tool: lotus-taint
==============================

The ``lotus-taint`` tool provides an interprocedural taint analysis using the
IFDS framework.

.. code-block:: bash

   ./build/bin/lotus-taint [options] <input bitcode file>

Key Options
-----------

* ``-analysis=<N>`` – Select analysis type (0=taint, 1=reaching-defs, default: 0)
* ``-sources=<functions>`` – Comma-separated list of custom source functions
* ``-sinks=<functions>`` – Comma-separated list of custom sink functions
* ``-show-results`` – Show detailed analysis results (default: true)
* ``-max-results=<N>`` – Maximum number of detailed results to show (default: 10)
* ``-verbose`` – Enable verbose output

The tool performs interprocedural taint analysis to detect potential security
vulnerabilities where tainted data (from sources like user input) flows to
dangerous sink functions (like system calls, memory operations, etc.).

Examples
--------

.. code-block:: bash

   # Basic taint analysis
   ./build/bin/lotus-taint input.bc

   # Custom sources and sinks
   ./build/bin/lotus-taint -sources="read,scanf" -sinks="system,exec" input.bc

   # Reaching definitions analysis
   ./build/bin/lotus-taint -analysis=1 input.bc

For other command-line tools that build on IFDS/IDE, see
:doc:`../tools/checker/index`.

