Paper Engines: PEARL, Stg, and Sqid
===================================

Lotus contains faithful, SVF-independent implementations of three recent
classical CFL-reachability algorithms:

* PEARL batches partially transitive relations and removes redundant
  propagation through sparse primary-edge graphs.
* Stg separates context-free summary discovery from ordered regular-language
  propagation.
* Sqid combines adaptive pivot selection with differential relation chaining.

The following pages document each source paper, its motivating problem and key
idea, the published algorithms, Lotus-specific adaptations, source locations,
and validation coverage.

Coverage summary
----------------

* PEARL: the shared standard baseline (Algorithm 1), the overall
  multi-derivation solver (Algorithm 2), packed partial propagation
  (Algorithm 3), full-transitive propagation graphs (Algorithm 4), inverse
  relations, and completeness Option 2.
* Stg: CFP-based grammar decomposition, standard and extended Dyck Phase L,
  the Alias-CFP worklist solver (Algorithm 1), regular ordered propagation
  (Algorithm 2), DNF alternatives, and nested-expression lowering.
* Sqid: the shared standard baseline (Algorithm 1), adaptive chaining
  (Algorithm 2), synchronized derivation/insertion (Algorithm 3), and the
  differential dual-worklist solver (Algorithm 4).

.. toctree::
   :maxdepth: 1

   pearl
   stg
   sqid
