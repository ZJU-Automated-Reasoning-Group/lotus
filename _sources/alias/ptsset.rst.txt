Points-To Set Backends
======================

``include/Alias/Infrastructure/PtsSet/`` contains alternative data structures for representing
points-to sets inside alias analyses.

**Backends in this tree**:

- ``DenseHashPtsSet`` for hash-based storage.
- ``ChunkedSparseBitsetPtsSet`` and ``BloomBitsetPtsSet`` for compact bitset
  variants.
- ``BDDAndersPtsSet`` for a BDD-backed representation.

Some of these implementations are experimental or selectively wired into the
current analyses, but the directory is the natural home for set-representation
experiments.

Common set interface
--------------------

The backends are designed around the operations used in propagation loops:
membership tests, insertion, union, subset checks, intersection tests,
iteration, clearing, and size queries.  In particular, ``insert`` and
``unionWith`` report whether the set changed, allowing a solver to enqueue
dependent constraints only when new points-to facts are discovered.

Choosing a representation
-------------------------

``DenseHashPtsSet`` is a simple baseline built on ``llvm::DenseSet`` and is a
good fit for irregular, sparse identifiers.  ``ChunkedSparseBitsetPtsSet``
stores only populated fixed-size chunks, which favors sparse sets with large
identifier ranges.  ``BloomBitsetPtsSet`` combines an exact dynamic bitset
with a Bloom filter that can accelerate negative membership and intersection
checks; the bitset remains the authority, so Bloom-filter false positives do
not change results.  ``BDDAndersPtsSet`` uses CUDD and supports BDD variable
reordering, trading a heavier dependency for compact symbolic representation
when it suits a workload.

The header comments identify which alternatives are currently standalone.
Before replacing a production backend, benchmark representative inputs and
verify that the surrounding solver relies only on operations implemented by
the candidate set.

See also :doc:`alias_analysis` and :doc:`metrics`.
