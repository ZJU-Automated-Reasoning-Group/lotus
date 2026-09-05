Taint Configuration Format and API
==================================

The ``include/Annotation/Taint/`` headers define Lotus's taint specification
format and the runtime API used to query sources, sinks, ignored functions, and
propagation rules.

**Location**: ``include/Annotation/Taint/``

**Main APIs**:

- ``TaintSpec`` describes a taint source or sink location.
- ``PipeSpec`` models taint propagation between positions.
- ``FunctionTaintConfig`` and ``TaintConfig`` store parsed specifications.
- ``TaintConfigParser`` parses ``.spec`` files.
- ``TaintConfigManager`` provides a shared lookup interface.

**Typical use cases**:

- Mark external functions as taint sources or sinks.
- Define sanitizer-like propagation behavior.
- Reuse the same taint model across IFDS analyses and security checkers.

Configuration workflow
----------------------

Keep specifications close to the API boundary they model: identify the
function, then describe source, sink, or propagation positions using the
configuration types.  Parse the file once with ``TaintConfigParser`` and pass
the resulting configuration through ``TaintConfigManager`` so all analysis
components apply the same model.  Treat a missing external summary
conservatively when the checker requires sound reporting.

See also :doc:`annotation`, :doc:`../checker/index`, and :doc:`../dataflow/ifds_ide`.
