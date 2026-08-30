Solvers
=======

This section documents the solver frameworks, constraint solving backends, and
SMT-based model checking components used throughout Lotus.

The available backends include BDD (CUDD), SMT (Z3-based), weighted pushdown
systems (WPDS), string constraint solving (Stingx), fixed-point equation
solving (FPsolve), and experimental solver tooling (TUNA, STAUB, SymAbs,
SMTSampler).

.. toctree::
   :maxdepth: 2

   cudd
   smt
   wpds
   smtsampler
   symabs
   staub
   stingx
   egraphs_simp
   fpsolve
   libsmt
   datalog

Third-Party Libraries
---------------------

Additional vendored libraries in ``third-party/`` that are not currently
integrated:

* **MDE** (``third-party/mde/``) — Multilevel Deduplication Engine
  for caching set operations in dataflow analyses. Currently not
  compiled or linked (commented out in ``third-party/CMakeLists.txt``).
