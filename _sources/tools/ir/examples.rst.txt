PDG Query Example Cookbook
==========================

``tools/ir/examples/`` contains ready-to-run Cypher queries for ``lotus-ir-pdg-query``.

**Location**: ``tools/ir/examples/``

**Contents**:

- ``primitives.cypher`` for basic node and edge exploration.
- ``dataflow.cypher`` and ``security.cypher`` for flow-oriented triage.
- ``metrics.cypher`` and ``debug-info.cypher`` for inspection and reporting.
- ``static-analysis.cypher`` for analysis-oriented query patterns.
- ``agent-guidance.cypher`` for LLM-agent-guided PDG exploration primitives.
- ``security/`` — categorized security analysis patterns:

  + ``injection.cypher`` — Command injection: system/popen/exec sinks + input source tracing
  + ``memory.cypher`` — Use-after-free, double-free, memory leaks
  + ``unsafe-libc.cypher`` — strcpy/gets/sprintf, format string, buffer overflow
  + ``resource.cypher`` — File handle leaks, lock/unlock, mmap/munmap
  + ``double-free.cypher`` — Double-free, use-after-free, new/delete mismatch
  + ``taint.cypher`` — Input-to-sink taint tracking, format string, argument tracing

The directory acts as a practical cookbook for users learning the PDG query
language.

How to use the examples
-----------------------

Start with ``primitives.cypher`` to inspect the graph schema and verify that a
PDG was imported as expected.  Then adapt a focused query such as a dataflow or
security pattern to the node labels and properties in the target graph.  Treat
the security examples as triage starting points: inspect their matches and
refine sources, sinks, and path constraints before reporting a finding.

See also :doc:`index` and :doc:`../../user_guide/pdg_query_language`.
