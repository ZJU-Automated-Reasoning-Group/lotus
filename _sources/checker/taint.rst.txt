IFDS Taint Checker
==================

The ``--engine=taint`` mode runs Lotus's interprocedural IFDS taint analysis.
It follows user-configured data from source functions to sink functions.

**Frontend**: ``lotus-check --engine=taint``

**Implementation**: ``tools/checker/lotus-check-taint.cpp`` and
``include/Dataflow/IFDS/Analyses/IFDSTaintAnalysis.h``

Scope
-----

Use this checker for configurable source-to-sink queries, such as untrusted
input reaching a command-execution API.  Supply project-specific sources and
sinks when the defaults do not describe the program.

This is distinct from the auxiliary taint facts used by ``kint`` to focus
numerical-bug analysis, and from the taint tracking embedded in ``pulse`` and
``symex``.  Those engines report their own bug classes; they are not a
replacement for an explicitly configured IFDS source-to-sink query.

Usage
-----

.. code-block:: bash

   # Run with the default source and sink configuration.
   ./build/bin/lotus-check --engine=taint input.bc

   # Add project-specific source and sink functions.
   ./build/bin/lotus-check --engine=taint input.bc \
     --taint.sources=recv,getenv --taint.sinks=system,execve

   # Select an alias-analysis backend and show source/sink tagging details.
   ./build/bin/lotus-check --engine=taint input.bc --taint.alias-analysis=dyck --log-level=debug

Important options
-----------------

* ``--taint.sources=<name[,name...]>`` adds source functions.
* ``--taint.sinks=<name[,name...]>`` adds sink functions.
* ``--taint.alias-analysis=<kind>`` selects the alias-analysis backend; the default is ``dyck``.
* ``--log-level=debug`` prints module details and source/sink tagging at call sites.
* ``--verbose`` prints detailed finding traces.
* ``--analysis-stats`` prints analysis statistics.
* ``--taint.micro-bench`` adds the conventional ``source`` and ``sink`` functions
  for benchmark evaluation.

Reporting
---------

The IFDS frontend exports findings through ``BugReportMgr`` and supports the
shared text, JSON, SARIF, suppression, confidence, and exit-status options.

See also
--------

* :ref:`Choosing a Checker <choosing-a-checker>` – bug-class to engine guide
* :doc:`../dataflow/ifds_ide` – IFDS/IDE framework
* :doc:`../annotation/taint_config` – reusable taint configuration
