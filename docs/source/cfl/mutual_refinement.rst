Mutual Refinement for CFL Reachability
======================================

``include/CFL/MutualRefinement/`` and ``lib/CFL/MutualRefinement/`` contain a
grammar-agnostic CNF reachability and derivation-tracing engine. It is a
low-level dependency of :doc:`interleaved_dyck_approximation`, not a second
domain-facing interleaved-Dyck solver.

**Location**: ``include/CFL/MutualRefinement/``,
``lib/CFL/MutualRefinement/``

**Main components**:

- ``CnfGrammar`` stores the integer-encoded grammar.
- ``CnfGraph`` stores the encoded graph instance.
- ``IntPairHasher`` supports the compact map/set structures used internally.
- ``MutualRefinementMain.cpp`` provides the standalone driver.

The reusable operations are ordinary CFL saturation, saturation with
unary/binary derivation records, and backward closure from derived results to
the original edges that contributed to them. The client supplies the grammar,
integer graph encoding, refinement schedule, and result semantics.

Tracing the contributing edges comes in two modes. The eager mode, which is
the default, records unary and binary derivations during saturation and walks
those records backward to the original edges. The opt-in factorized mode runs
ordinary saturation, which allocates no provenance records, and then lazily
reconstructs the contributing edges from the saturated relations. It builds
factorized ``Out_X``/``In_X`` views of the closure and probes the smaller
relation at each binary join.

``InterleavedDyckApproximation`` exposes the same choice through
``Options::factorized_tracing``, which defaults to ``false``; see
:doc:`interleaved_dyck_approximation`.

``MutualRefinementMain.cpp`` additionally preserves the original generic
file-driven experiment and its alternating refinement loop. It treats parsed
labels as opaque grammar symbols; it does not assign parenthesis/bracket
meaning or expose the application pipeline implemented by
``InterleavedDyckApproximation``.

Responsibility Boundary
-----------------------

``MutualRefinement`` does **not** own:

- typed ``op/cp/ob/cb`` label parsing;
- projected, union-Dyck, parity, or endpoint grammar selection;
- regularization, condensation, or on-demand policy;
- taint and value-flow benchmark semantics; or
- lower-bound versus upper-bound interpretation.

Those responsibilities belong to
:doc:`interleaved_dyck_approximation`, which translates its structured graph
into the integer representation here and uses the derivation records to decide
which original edges survive the next refinement round.

The implementation is best treated as a focused research component within the
broader CFL subsystem.

Experiment workflow
-------------------

Prepare the grammar and graph using the integer encodings expected by the
component, then run the standalone driver to evaluate the refinement process.
The local ``Grammar`` and ``Graph`` types are intentionally specialized; use a
different CFL frontend when an application needs a stable LLVM-facing API or
human-readable input format.  Record the encoding and benchmark corpus when
comparing refinement strategies.

.. code-block:: console

   cmake --build build --target lotus-cfl-mutual-refinement
   build/bin/lotus-cfl-mutual-refinement grammars.txt graph.dot refine
   build/bin/lotus-cfl-mutual-refinement grammars.txt graph.dot refine --factorized-tracing

The mode is ``naive`` or ``refine``. Grammar symbols and graph labels are
opaque strings that are encoded to integers before invoking ``CnfGraph``.
Pass ``--factorized-tracing`` after the mode to replace eager derivation
records with lazy reconstruction from the saturated relations; without it the
driver keeps the original eager records.

See also :doc:`cfl_components`.
