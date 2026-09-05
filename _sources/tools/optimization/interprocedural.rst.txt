Inter-Procedural Optimizations
==============================

This page summarizes inter-procedural optimizations implemented in
``lib/Optimization/IPO/`` and exposed via the ``lotus-opt-ipo`` tool.

**Implementation Location**: ``lib/Optimization/IPO/``

Tool
----

- **Binary**: ``lotus-opt-ipo``
- **Source**: ``tools/optimization/lotus-opt-ipo.cpp``
- **Selection flags**: ``-ipdse``, ``-ip-rle``, ``-ip-sink``, ``-ip-forward``,
  or ``-ip-all`` to run all of them.

Example:

.. code-block:: bash

   build/bin/lotus-opt-ipo -ip-all input.bc -o optimized.bc

Passes
------

- **IPDeadStoreElimination** (``lib/Optimization/IPO/IPDeadStoreElimination.cpp``)
  Removes inter-procedurally provable dead stores.
- **IPRedundantLoadElimination** (``lib/Optimization/IPO/IPRedundantLoadElimination.cpp``)
  Eliminates redundant loads across function boundaries when safe.
- **IPStoreSinking** (``lib/Optimization/IPO/IPStoreSinking.cpp``)
  Sinks stores inter-procedurally to reduce redundant writes.
- **IPStoreToLoadForwarding** (``lib/Optimization/IPO/IPStoreToLoadForwarding.cpp``)
  Forwards values from stores to later loads across calls when possible.

Notes
-----

- ``lib/Optimization/README.md`` tracks optimization-specific references and
  evaluation notes for select passes (e.g., prefetching, LICM).
- :doc:`../../optimization/ip` documents the dedicated ``lib/Optimization/IPO/``
  subtree.
