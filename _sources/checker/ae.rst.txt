Abstract Execution Checker
==========================

The ``AE`` subsystem provides abstract-execution-based bug detection.

**Headers**: ``include/Checker/AE/``

**Implementation**: ``lib/Checker/AE/``

**Frontend**: ``lotus-check --engine=ae`` implemented by ``tools/checker/lotus-check-ae.cpp``

Overview
--------

AE runs abstract interpretation and specialized detectors to report memory
safety issues such as buffer overflows, null dereferences, use-after-free, and
invalid frees.

Main components
---------------

- ``AbstractInterpretation`` drives the fixpoint analysis.
- ``AEDetector`` and its concrete detectors implement bug-specific checks.
- Value abstractions such as ``AbstractValue``, ``NumericValue``, and
  ``AbstractState`` model execution state during analysis.
- ``RelationSolver`` and related utilities support symbolic reasoning in the
  abstract domain.

Checks supported
----------------

- Buffer overflow
- Null pointer dereference
- Use-after-free
- Invalid free
- Memory leak

Command-line usage
------------------

.. code-block:: bash

   ./build/bin/lotus-check --engine=ae input.bc --checks=all
   ./build/bin/lotus-check --engine=ae input.bc --checks=buffer-overflow,null-deref
   ./build/bin/lotus-check --engine=ae input.bc --ae.recursion-mode=widen-narrow --ae.widen-delay-iterations=5

Important options
-----------------

- ``--checks=<id[,id...]>`` selects ``buffer-overflow``, ``null-deref``,
  ``use-after-free``, ``invalid-free``, or ``memory-leak``. Omitting it, or
  passing ``--checks=all``, enables all AE checks.
- ``--ae.recursion-mode`` chooses the recursion strategy.
- ``--ae.widen-delay-iterations`` delays widening in loops.
- ``--verbose`` prints more detailed traces for findings.

Scope and alternatives
----------------------

``ae`` is a broad abstract-execution memory-safety pass. Its ``--checks`` option
enables only the five AE detectors listed above; it does not run Pulse, FiTx,
Saber, or SymEx.  Use ``fitx`` for faster translation-unit feedback, ``pulse``
for bounded witness-oriented diagnosis, ``saber`` for leaks and double frees
over sparse value flow, and ``symex`` for SMT-backed path-sensitive checks.
See :ref:`Choosing a Checker <choosing-a-checker>` for the bug-class guide.

See also
--------

- See :doc:`../tools/checker/index` for the front-end overview.
- See :doc:`pulse` and :doc:`kint` for other checker families.
