Algebraic Program Analysis (APA)
=============================

``APA`` is Lotus's elimination-based dataflow analysis (or, "algebraic program analysis") framework.

**Headers**: ``include/Dataflow/APA/``

**Implementation**: ``lib/Dataflow/APA/``

Overview
--------

APA expresses classical dataflow problems as algebraic program analysis and solves
them with elimination-style algorithms. The framework currently provides LLVM
clients for standard intraprocedural analyses and reusable solver backends.

Main components
---------------

- ``Core/`` defines generic problem, result, and option abstractions.
- ``Solver/`` contains solver implementations such as state elimination,
  ADT-simple, and ADT-delayed solvers.
- ``Domains/`` defines the abstract fact types and their lattice operations.
- ``LLVM/`` bridges LLVM CFGs, dominance information, and call resolution to
  the generic APA problem interfaces.
- ``Analyses/Intra/`` provides ready-made intraprocedural analyses:

  - available expressions
  - constant propagation
  - live variables
  - lockset analysis
  - non-null
  - reachability
  - reaching definitions
  - sign analysis
  - uninitialized variables
  - very busy expressions

- ``Analyses/Inter/`` contains the supported interprocedural variants. Domains
  that currently support only one scope simply have no analysis entry point in
  the other directory.

- ``Passes/EliminationPasses.h`` exposes LLVM-pass integration.

Interprocedural Forward Summary Solver
--------------------------------------

The ``ForwardInterSummarySolver`` is a context-sensitive interprocedural
solver that encodes the entire program as a single global equation graph.
It solves the graph by computing SCCs and evaluating closed-form regular
path expressions, rather than iterating per-procedure worklists to
convergence.

Conceptual model
^^^^^^^^^^^^^^^^

The solver builds a ``PathSummaryEquationGraph`` whose nodes are
``(Instruction, CallStringContext)`` pairs. Edges are labeled with
``InterSummaryTransferAtom`` values that reify interprocedural transfer
effects. There are four atom kinds:

- ``RawNormal`` — ordinary intraprocedural edge transfer.
- ``CallEntry`` — transfer from a call site into a callee entry point
  (``callFlow`` in the problem interface).
- ``ReturnExit`` — transfer from a callee exit back to a return site
  (``returnFlow``).
- ``CallToRet`` — bypass transfer that skips the callee and flows
  directly from call site to return site (``callToRetFlow``).

The resulting system of left-linear equations has the form:

.. code-block:: text

  X_u = base_u  U  (W_u,v . X_v)

where ``base_u`` captures contributions from inter-SCC predecessors and
seed facts, and ``W_u,v`` are path expressions composed from the atom
types above.

Contrast with the worklist solver
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

APA provides two interprocedural solver engines. The worklist-based
``InterEliminationSolver`` wraps each function as an intraprocedural
problem and propagates across call sites through a global worklist.
The summary-based ``ForwardInterSummarySolver`` takes a different
approach:

+-----------------------------------+------------------------------------+
| Worklist solver                   | Summary solver                     |
| (``InterEliminationSolver``)      | (``ForwardInterSummarySolver``)    |
+===================================+====================================+
| Solves one procedure at a time    | Builds one global equation graph   |
| via ``ProcedureProblemAdapter``   | covering all (inst, context) nodes |
+-----------------------------------+------------------------------------+
| Converges by iterating the        | Closes recursive SCCs with a       |
| worklist until facts stabilize    | ``Star`` expression operator       |
+-----------------------------------+------------------------------------+
| Sequential per-procedure solving  | Solves SCCs in dependency order    |
+-----------------------------------+------------------------------------+
| No SCC decomposition              | Tarjan SCC decomposition followed  |
|                                   | by topological layer ordering      |
+-----------------------------------+------------------------------------+
| Cyclic call graphs handled by     | Cyclic SCCs use Floyd-Warshall-    |
| re-enqueuing changed nodes        | style closure over path expressions|
+-----------------------------------+------------------------------------+
| ``lotus-dfa-apa`` tool uses this  | Library-only API, no tool frontend |
| engine by default                 | (use the ``runInterSummaryElim*``  |
|                                   | functions directly)                |
+-----------------------------------+------------------------------------+

Supported analyses
^^^^^^^^^^^^^^^^^^

Five interprocedural analysis entry points provide both worklist and
summary solver variants:

``runInterSummaryElimReachable``
  ``(Function *Entry, const InterCFG *, PathSummaryEquationOptions)``

``runInterSummaryElimConstantPropagation``
  ``(Function *Entry, AAResults *, AssumptionCache *, DominatorTree *,``
  ``TargetLibraryInfo *, const InterCFG *, PathSummaryEquationOptions)``

``runInterSummaryElimReachingDefinitions``
  ``(Function *Entry, AAResults *, MemorySSA *, const InterCFG *,``
  ``PathSummaryEquationOptions)``

``runInterSummaryElimUninitVariables``
  ``(Function *Entry, AAResults *, AssumptionCache *, DominatorTree *,``
  ``const InterCFG *, PathSummaryEquationOptions)``

``runInterSummaryElimLockset``
  ``(Function *Entry, const InterCFG *, PathSummaryEquationOptions)``

Each pair-solver variant shares the same analysis domain and fact types
with its worklist counterpart, making it straightforward to switch
between engines for differential validation.

Key classes
^^^^^^^^^^^

``ForwardInterSummarySolver<AnalysisDomainTy, K>``
  Main solver template. Owns a ``PathSummaryEquationGraph``, discovers
  equation nodes by traversing the interprocedural CFG from seed facts,
  delegates to ``PathSummaryEquationSolver`` for solving, and evaluates
  the resulting summaries into ``InterDataFlowResultT`` facts. Lives in
  ``include/Dataflow/APA/Solver/ForwardInterSummarySolver.h``.

``PathSummaryEquationSolver<KeyT, TransferT>``
  Generic engine that computes Tarjan SCCs on the equation graph, orders
  them by dependency, and solves them sequentially. Cyclic SCCs are solved
  with a Floyd-Warshall-style closure over path expressions using ``Star``,
  ``Concat``, and ``Union`` operators. Lives in
  ``include/Dataflow/APA/Solver/PathSummaryEquationSolver.h``.

``InterSummaryTransferAtom<AnalysisDomainTy>``
  First-class atom that tags an edge as one of the four transfer kinds
  (RawNormal, CallEntry, ReturnExit, CallToRet). Constructed via static
  factory methods (``rawNormal``, ``callEntry``, ``returnExit``,
  ``callToRet``).

``InterSummaryTransferEvaluator<AnalysisDomainTy, K>``
  Evaluates a path expression built from ``InterSummaryTransferAtom``
  labels against an input fact. Dispatches each atom kind to the
  corresponding problem hook (``applyTransfer``, ``callFlow``,
  ``returnFlow``, ``callToRetFlow``) and handles ``Star`` by iterating
  until fixpoint.

Usage example
^^^^^^^^^^^^^

.. code-block:: cpp

  #include "Dataflow/APA/APA.h"

  using namespace elimination;

  // Build the interprocedural CFG for the module.
  auto ICF = dataflow::controlflow::InterCFG::build(*Module);

  // Run the summary-based reachability analysis.
  auto Result = runInterSummaryElimReachable(Main, ICF.get());

  // Inspect the result at a program point.
  for (auto &Inst : instructions(*Main)) {
    if (auto *In = Result.tryIN(&Inst, {})) {
      // Inst is reachable from the entry point.
    }
  }

  // Diagnostics include equation graph statistics.
  auto &Diag = Result.summarySolveDiagnostics();
  assert(Diag.equation_node_count > 0);
  assert(Diag.scc_count > 0);

The unit tests in ``tests/unit/Dataflow/APA/`` validate parity between the
worklist and summary solvers across the supported analysis types, including
recursive call-graph patterns.

Library-only availability
^^^^^^^^^^^^^^^^^^^^^^^^^^

The ``lotus-dfa-apa`` command-line tool currently uses the worklist-based
``InterEliminationSolver``. The summary solver is available as a
library-only API. To use it, call the ``runInterSummaryElim*`` functions
directly from your own pass or analysis driver.

Typical use cases
-----------------

- Compare elimination-based solving with Mono or IFDS engines.
- Prototype intraprocedural dataflow problems over LLVM IR.
- Drive differential-testing workflows for solver validation.

See also
--------

- See :doc:`mono`, :doc:`ifds_ide`, and :doc:`npa` for related engines.
- See :doc:`../tools/dataflow/index` for the testing front-ends.
