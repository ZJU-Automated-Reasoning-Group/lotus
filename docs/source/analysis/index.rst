Analysis Framework
==================

This section covers the core analysis components and frameworks in Lotus.

Lotus provides several reusable analysis utilities and frameworks under
``lib/Analysis`` and adjacent subsystem modules such as ``lib/Concurrency``.
These components complement the alias analyses and high-level analyzers such
as CLAM (numerical abstract interpretation) and SymAbsAI (symbolic
abstraction + abstract interpretation) built in ``lib/Verification``.

Overview
--------

At a glance:

- **CFG** (``lib/Analysis/CFG``): Control Flow Graph utilities for reachability,
  dominance, and structural reasoning. See :doc:`cfg`.
- **ControlDependence** (``lib/Analysis/ControlDependence``): Standard CD,
  NTSCD variants, DOD variants, strong control closure, and Lotus ICFG
  integration. See :doc:`control_dependence`.
- **Concurrency** (``lib/Concurrency``): Thread-aware analyses for
  multi-threaded code (MHP, lock sets, thread modeling). Now documented in its
  own section: see :doc:`../concurrency/index`.
- **DebugInfo** (``lib/Analysis/DebugInfo``): Source-location and metadata
  extraction support. See :doc:`debug_info`.
- **Loop** (``lib/Analysis/Loop``): Loop-dependence, iteration-space, and
  transformation-oriented loop analyses. See :doc:`loop`.
- **NullPointer** (``lib/Analysis/NullPointer``): A family of nullness and
  null-flow analyses. See :doc:`null_pointer`.
- **TypeHierarchy** (``lib/Analysis/TypeHierarchy``): Type-hierarchy and vtable
  recovery for object-oriented code. See :doc:`type_hierarchy`.
- **SCCP** (``lib/Analysis/SCCP``): Sparse conditional constant propagation
  using a three-valued lattice (Top/Constant/Bottom) to discover constants
  and dead code. See :doc:`sccp`.
- **ParameterSummary** (``lib/Analysis/ParameterSummary``): Per-function
  parameter effect summaries tracking freed, dereferenced, and allocated
  parameters, with transitive call-graph composition. See :doc:`parameter_summary`.
- **Multiplicity** (``lib/Analysis/Multiplicity``): Allocation multiplicity
  classification — classifies global, stack, and heap allocations as
  *Unique* or *Summary*. See :doc:`multiplicity`.
- **Purity** (``lib/Analysis/Purity``): Function-level purity and side-effect
  analysis — classifies functions as *Const*, *Pure*, *Impure*, or *Unknown*
  using attribute inference, MemorySSA summaries, and external summary stores.
  See :doc:`purity`.
- **Profile** (``lib/Analysis/Profile``): Profile-guided hot-code detection
  using LLVM's ``BlockFrequencyInfo`` and ``BranchProbabilityInfo`` for
  instruction, basic-block, and loop frequency queries. See :doc:`profile`.


Higher-level analyzers such as CLAM and SymAbsAI build on these components;
see :doc:`../verification/clam` and :doc:`../verification/symabs-ai` for
details.

.. toctree::
   :maxdepth: 2

   cfg
   control_dependence
   debug_info
   loop
   null_pointer
   type_hierarchy
   sccp
   parameter_summary
   multiplicity
   purity
   profile
