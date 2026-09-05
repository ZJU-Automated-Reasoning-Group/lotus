DFPA
====

``DFPA`` is a demand-refined function-pointer analysis.

**Headers**: ``include/Alias/Specialized/DFPA/``

**Implementation**: ``lib/Alias/Specialized/DFPA/``

**Tool**: the standalone ``dfpa`` frontend is no longer present in this tree; invoke DFPA via ``lotus-alias-call-graph -cg-type=dfpa``

Overview
--------

DFPA starts from coarse indirect-call candidates and refines them on demand
using program indexing plus selective context sensitivity. It is aimed at
resolving function-pointer calls more precisely than simple type-based or
flow-insensitive baselines without paying the full cost of a highly precise
whole-program pointer analysis everywhere.

Key pieces
----------

- ``ProgramIndex`` builds abstract objects and slot keys for the analyzed IR.
- ``DFPAPass`` runs the refinement algorithm and records refined targets.
- ``DFPAResult`` exposes call-target sets and summary statistics.

Configuration knobs
-------------------

The pass exposes several important tuning parameters (also configurable via ``lotus-alias-call-graph -cg-type=dfpa``):

- ``indirect_ctx_k``: selective context depth on indirect edges.
- ``refine_ambiguous_only``: focus demand refinement on unresolved calls.
- ``max_offset_depth``: bound offset-path exploration.
- ``max_demand_states``: cap refinement-state growth.
- ``enable_signature_filter``: intersect candidates with normalized signatures.

Typical workflow
----------------

1. Run the preprocessing pipeline that canonicalizes GEPs, lowers memcpy, and
   normalizes exception-handling and heap-model details.
2. Build the ``ProgramIndex``.
3. Execute ``DFPAPass`` with the desired demand budget.
4. Consume refined indirect-call targets from ``DFPAResult``.

See also
--------

- See :doc:`../tools/alias/index` for the command-line front-end.
- See :doc:`fpa` and :doc:`tpa` for other indirect-call resolution strategies.
