Interleaved-Dyck Graph Reduction
================================

``GraphReduction`` packages the PLDI 2020 graph-simplification pipeline for
interleaved-Dyck reachability. It transforms a DOT graph; it does not itself
answer the final reachability relation.

**Location**: ``lib/CFL/InterleavedDyck/GraphReduction/``

Pipeline
--------

The Python driver alternates two compiled phases:

``lotus-cfl-interleaved-dyck-graphaux``
   Builds one-color summary components and emits the color-reach graph.

``lotus-cfl-interleaved-dyck-dkmerge``
   Merges nodes using the specialized degree/color data structure and records
   edges that cannot be removed.

``lotus-cfl-interleaved-dyck-graph-reduction.py``
   Orchestrates both colors until no further edge is removed. The input file is
   updated in place, so experiments should operate on a copy.

.. code-block:: console

   cmake --build build --target lotus-cfl-interleaved-dyck-graph-reduction
   cp input.dot reduced.dot
   python3 build/bin/lotus-cfl-interleaved-dyck-graph-reduction.py reduced.dot \
     --graphaux build/bin/lotus-cfl-interleaved-dyck-graphaux \
     --dkmerge build/bin/lotus-cfl-interleaved-dyck-dkmerge

Bidirected handling
-------------------

The shared typed graph and the approximation/MCFL solvers preserve input arcs
as supplied. This reducer has specialized internal orientation rules: closing
colored edges are stored in reverse orientation, and a legacy one-color CFL
construction can create synthetic reverse terminals. These are internal
summary semantics, not silent bidirecting of the shared input. Pass
``--bidirected-input`` only when the dataset already represents both
directions.

Implementation boundary
-----------------------

Artifact-era ``CFLGraph``, ``CFLReach``, ``SummaryGraph``, and merge-list types
live under ``lib/CFL/InterleavedDyck/GraphReduction/Legacy`` as private implementation
details. They model intermediate color summaries and merge bookkeeping and
therefore should not be unified with the immutable input representation in
``interleaved_dyck::Graph``.

See also :doc:`cfl_components`.
