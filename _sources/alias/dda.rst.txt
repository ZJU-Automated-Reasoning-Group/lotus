DDA
===

``DDA`` provides demand-driven alias-analysis infrastructure.

**Headers**: ``include/Alias/DemandDriven/DDA/``

**Implementation**: ``lib/Alias/DemandDriven/DDA/``

Overview
--------

The DDA subsystem contains solver support for refining alias information on
demand instead of materializing a full global solution up front. This is useful
when a client only needs answers for a small set of pointer queries.

Main components
---------------

- ``DDAVFSolver`` implements the core demand-driven value-flow solving logic.
- The subsystem is intended as reusable analysis infrastructure rather than a
  user-facing standalone tool.

When to use it
--------------

- Clients need selective alias or value-flow refinement.
- Full whole-program pointer analysis is too expensive for the task at hand.
- A checker or transformation wants to escalate precision only around hot spots.

See also
--------

- See :doc:`dfpa` for a function-pointer focused demand-refined analysis tool.
- See :doc:`dynaa` for dynamic validation of static aliasing results.
