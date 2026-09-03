===============================
Flow-Sensitive Pointer Analysis
===============================

Overview
========

``FlowSensitivePTA`` is a **sparse, flow-sensitive, inclusion-based** pointer
analysis. Conventional inclusion-based (Andersen-style) analyses are
flow-insensitive: they compute a single points-to set per pointer, merged over
all program points. The flow-sensitive solver instead tracks points-to facts
at each program point, and it is *sparse* because it solves over the Sparse
Value-Flow Graph (SVFG) and its MemorySSA rather than re-analyzing every
program point.

The solver maintains:

* top-level points-to sets for pointer-producing SVFG nodes;
* per-node, per-object MemorySSA ``IN`` and ``OUT`` points-to state;
* explicit Addr/Copy/GEP/Phi/Load/Store and parameter-flow transfer;
* canonical ``(allocation, normalized byte offset)`` field objects, with array
  indices collapsed for field-insensitive updates;
* field-offset-aware aggregate global initializers and ``memcpy``/``memmove``;
* singleton-object strong updates and conservative weak updates;
* Tarjan SCC decomposition with SCC-local fixed points and successor
  requeueing;
* auxiliary-PTA call-graph initialization plus on-the-fly indirect-call
  connection followed by SCC reconstruction;
* selectable mutable and hash-consed points-to set storage.

Location
========

* ``include/Alias/InclusionBased/FlowSensitive/FlowSensitivePTA.h``
* ``include/Alias/InclusionBased/FlowSensitive/VersionedFlowSensitivePTA.h``
* ``lib/Alias/InclusionBased/FlowSensitive/``

Components
==========

The module contains Lotus-native migrations of both SVF pipelines:

1. **FlowSensitivePTA** implements the default exhaustive ``fspta`` analysis.
   It is the thread-independent sparse solver described above.

2. **VersionedFlowSensitivePTA** implements the ``vfspta`` analysis. Memory
   facts are keyed by ``(abstract object, meld version)`` rather than by
   ``(SVFG location, abstract object)`` as in the conventional solver. It adds
   object prelabeling, meld versions, consume/yield maps, version and statement
   reliance, strong and weak updates, intrinsic memory definitions,
   footprint-equivalent object reuse, occurrence-weighted propagation, OTF
   delta-edge updates, and result persistence.

Inputs
------

Both solvers operate on the Lotus SVFG built from an ICFG. The driver builds
the ICFG with ``ICFGBuilder`` and the SVFG with ``SVFGBuilder``, enabling
MemorySSA construction and a selectable memory-region partition strategy, then
connects pre-analysis indirect calls before solving.

The concurrency layer does not duplicate this solver. ``FSMPTA`` runs it over
an SVFG augmented with fork/join and ``ThreadMHPIndirectVF`` edges, and MSli
supplies an optional filtered solve graph.

Usage
=====

The ``lotus-alias-fspta`` driver runs either solver on an LLVM bitcode module:

.. code-block:: bash

   ./build/bin/lotus-alias-fspta input.bc
   ./build/bin/lotus-alias-fspta input.bc --analysis=vfspta --print-pts
   ./build/bin/lotus-alias-fspta input.bc --points-to-sets=hash-consed --dump-stats
   ./build/bin/lotus-alias-fspta input.bc --dump-svfg=fspta.dot --print-memory

Key options:

* ``--analysis=fspta|vfspta`` – Select the conventional sparse flow-sensitive
  solver (default) or the object-versioned solver.
* ``--points-to-sets=mutable|hash-consed`` – Points-to set backend: mutable
  ordered sets (default) or interned immutable sets with operation caching.
* ``--memory-partition=distinct|intra-disjoint|inter-disjoint`` – MemorySSA
  region partition strategy (default ``inter-disjoint``).
* ``--print-pts`` – Print top-level points-to results.
* ``--print-memory`` – Print non-empty sparse memory facts.
* ``--dump-stats`` – Print solver statistics (default on).
* ``--dump-svfg=<file>`` – Write the initialized SVFG as a DOT file.
* ``--validate-annotations`` – Validate ``__aser_alias__``/``__aser_no_alias__``
  calls against the analysis result.

See also :doc:`../tools/alias/index` for the tool reference.