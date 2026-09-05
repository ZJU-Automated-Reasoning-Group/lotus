Debug Info
==========

``DebugInfo`` extracts and normalizes source-level metadata from LLVM IR.

**Headers**: ``include/Analysis/DebugInfo/``

**Implementation**: ``lib/Analysis/DebugInfo/``

Overview
--------

This subsystem bridges LLVM debug metadata and Lotus analyses. It is used by
bug checkers and reporting code that need stable source locations, loop
structure, and metadata-derived identities.

Main components
---------------

- ``DebugInfoAnalysis`` extracts reusable debug information from a module.
- ``MetadataManager`` utilities provide instruction-, module-, and
  annotation-level metadata access.
- ``LoopStructure`` and ``MetadataEntry`` help analyses reason about source
  structure beyond raw IR.

Typical use cases
-----------------

- Attach file and line information to bug reports.
- Recover loop structure or source annotations from LLVM metadata.
- Support source-aware post-processing of analysis results.

Reliability considerations
--------------------------

Debug metadata may be incomplete or absent in optimized or stripped bitcode.
Clients should therefore retain an IR-level fallback identity and treat a
missing file or line as unavailable information, not as an analysis failure.
When emitting a report, use the normalized locations from this layer so
different checkers present source positions consistently.

See also
--------

- See :doc:`../checker/index` for checker components that rely on this data.
