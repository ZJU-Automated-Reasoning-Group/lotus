PDG Query – Program Dependence Graph Queries
==============================================

Interactive and batch query engine for the Program Dependence Graph (PDG).

**Binary**: ``lotus-ir-pdg-query``  
**Location**: ``tools/ir/lotus-ir-pdg-query.cpp``

**Usage**:

.. code-block:: bash

   # Interactive mode
   ./build/bin/lotus-ir-pdg-query -i program.bc

   # Single query
   ./build/bin/lotus-ir-pdg-query -q "MATCH (n:FUNC_ENTRY) WHERE n.name = 'main' RETURN n" program.bc

   # Batch queries from file
   ./build/bin/lotus-ir-pdg-query -f queries.txt program.bc

Key features:

- Forward/backward slicing
- Property-based slicing via ``--property-file``
- Information flow queries
- Security policy checks
- Subgraph export (DOT)

See :doc:`../../user_guide/pdg_query_language` for the language reference and
:doc:`examples` for the in-repo query cookbook.

Control-Dependence Driver
-------------------------

Standalone driver for the control-dependence algorithms in
``lib/Analysis/ControlDependence``. It runs exactly one algorithm per
invocation over every function in the input module (or a single function with
``--function``) and emits text, JSON, or CSV records with node/edge counts,
biclique statistics, exact pair counts, closure sizes, and per-phase timing.

**Binary**: ``lotus-ir-control-dependence``

**Location**: ``tools/ir/lotus-ir-control-dependence.cpp``

**Usage**:

.. code-block:: bash

   # Default: compact DOD preprocessing over every function
   ./build/bin/lotus-ir-control-dependence program.bc

   # Baseline vs compact NTSCD comparison
   ./build/bin/lotus-ir-control-dependence program.bc --algorithm=ntscd2 --format=csv
   ./build/bin/lotus-ir-control-dependence program.bc --algorithm=ntscd-compact --format=csv

   # DOD pair enumeration; pairs are counted, never printed or stored
   ./build/bin/lotus-ir-control-dependence program.bc --algorithm=dod-compact --visit-pairs

   # Strong closure from the entry plus an extra seed
   ./build/bin/lotus-ir-control-dependence program.bc --algorithm=strong-closure --seed-index=3 --format=json

   # Restrict to one function
   ./build/bin/lotus-ir-control-dependence program.bc --algorithm=dod --function=main

Relevant options:

- ``--algorithm=<name>`` selects one primitive algorithm operation. Valid names
  are ``ntscd2``, ``ntscd-compact``, ``dod``, ``dod-compact``,
  ``dod-compact-exact-set``, ``dod-ntscd``, ``dod-ntscd-compact``,
  ``strong-closure``, ``compact-closure``, and ``compact-closure-eager-pairs``.
- ``--visit-pairs`` is valid only for DOD algorithms; it traverses exact pairs
  through a counting callback and performs no per-pair output.
- ``--function=<name>`` restricts the experiment to one function.
- ``--seed-index=N`` adds closure seeds; the function entry is always included.
- ``--format=text|json|csv`` selects the output format.

.. toctree::
   :maxdepth: 1

   examples
