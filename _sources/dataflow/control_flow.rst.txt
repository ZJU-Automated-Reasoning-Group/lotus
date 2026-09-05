Control Flow Support
====================

``Dataflow/ControlFlow`` provides reusable CFG abstractions for dataflow engines.

**Headers**: ``include/Dataflow/ControlFlow/``

Overview
--------

This subsystem defines lightweight intraprocedural and interprocedural control
flow graph interfaces that decouple solver code from raw LLVM traversal.

Main components
---------------

- ``FlowDirection`` models forward versus backward analyses.
- ``IntraCFG`` and ``LLVMIntraCFG`` provide function-local control flow views.
- ``InterCFG`` and ``LLVMInterCFG`` extend the model across calls.

Why it exists
-------------

- Shared CFG abstractions reduce duplicated traversal logic across solvers.
- Engines can operate over a small stable interface instead of directly on LLVM.
- The same framework supports forward and backward analyses.

Choosing a graph view
---------------------

Use ``LLVMIntraCFG`` when facts remain within one function and an LLVM basic
block view is sufficient.  Use ``LLVMInterCFG`` only when transfers must cross
call and return boundaries; its added edges require the solver to define how
context and external calls are handled.  A client selects ``FlowDirection``
once so predecessor and successor traversal remains consistent throughout the
analysis.

See also
--------

- See :doc:`apa`, :doc:`mono`, and :doc:`ifds_ide` for clients built on top of
  control-flow abstractions.
