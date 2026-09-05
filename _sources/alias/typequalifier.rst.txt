TypeQualifier
=============

``TypeQualifier`` contains qualifier-based analysis infrastructure.

**Headers**: ``include/Alias/Specialized/TypeQualifier/``

**Implementation**: ``lib/Alias/Specialized/TypeQualifier/``

Overview
--------

This subsystem models qualifier-style properties over program values and
aggregates them interprocedurally. The codebase includes support for call-graph
construction, summaries, node factories, taint signatures, and qualifier
propagation utilities.

Main components
---------------

- ``QualifierAnalysis`` drives the analysis.
- ``FunctionSummary`` stores interprocedural summaries.
- ``CallGraphPass`` and related helpers manage the analysis call graph.
- ``TaintSignature`` and annotation helpers encode qualifier effects.

Use cases
---------

- Qualifier propagation across function boundaries.
- Lightweight value classification driven by summaries.
- Research prototypes for taint-like or effect-like properties.

See also
--------

- See :doc:`../annotation/annotation` for the annotation formats used elsewhere
  in the tree.
