Context-Sensitive Reachability Indexing
=======================================

``CSIndex`` is organized as two explicit subsystems:

.. code-block:: text

   SCS -> FLARE

**Location**: ``include/CFL/CSIndex/``, ``lib/CFL/CSIndex/``

FLARE
-----

``FLARE`` implements extended-Dyck indexing for context-sensitive
reachability. Its shared graph and algorithms live directly under
``CFL/CSIndex/FLARE``. Algorithm-owned code is grouped further:

* ``FLARE/Grail`` contains the GRAIL reachability index.
* ``FLARE/PathTree`` contains PathTree construction, querying, weighted graph
  support, and compression.
* ``FLARE/Tabulation`` contains sequential and parallel exact baselines.

The main public APIs are:

* ``lotus::cfl::cs_index::flare::Graph``;
* ``lotus::cfl::cs_index::flare::Index``;
* ``lotus::cfl::cs_index::flare::ReachBackbone``;
* ``lotus::cfl::cs_index::flare::grail::Index``;
* ``lotus::cfl::cs_index::flare::path_tree::Index`` and ``Query``; and
* ``lotus::cfl::cs_index::flare::tabulation::Sequential`` and ``Parallel``.

SCS
---

``SCS`` adds sanitizer-aware reachability. Every input edge has independent
structural and security-event labels. The policy automaton product is built
before FLARE's summary-edge and indexing transformations, ensuring both
constraints refer to the same witness path.

The public namespace is ``lotus::cfl::cs_index::scs`` and exposes ``Graph``,
``PolicyAutomaton``, ``Index``, and ``FactorizedIndex``. ``Index`` supports
explicit or source-rooted lazy product construction, point and fixed-batch
queries, optional witness replay, and construction/query statistics.

Build and tools
---------------

``CanaryCSIndexFLARE`` and ``CanaryCSIndexSCS`` are separate libraries; the
latter links the former. Configure ``LOTUS_ENABLE_CSR`` to build the FLARE
command-line driver:

.. code-block:: console

   cmake -S . -B build -DLOTUS_ENABLE_CSR=ON
   cmake --build build --target csr
   build/bin/csr input.graph

The driver supports GRAIL, PathTree, combined indexes, sequential tabulation,
and parallel tabulation. See :doc:`../tools/cfl/index` for command-line
options.
