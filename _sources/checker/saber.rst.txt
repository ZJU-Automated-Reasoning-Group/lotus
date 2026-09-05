Saber Checker
=============

The ``Saber`` subsystem implements source-sink bug checking on top of Lotus IR
and value-flow graphs.

**Headers**: ``include/Checker/Saber/``

**Implementation**: ``lib/Checker/Saber/``

**Frontend**: ``lotus-check --engine=saber`` implemented by ``tools/checker/lotus-check-saber.cpp``

Overview
--------

Saber-style analysis tracks source-sink relationships over the sparse
value-flow graph to detect resource-management bugs. In the current tree it is
used primarily for memory leaks, double-free bugs, and file-descriptor leaks.

Main components
---------------

- ``LeakChecker`` checks unmatched allocations and partial leaks.
- ``DoubleFreeChecker`` reports repeated frees on the same path.
- ``FileChecker`` handles file-descriptor style resources.
- ``SaberCheckerAPI`` and ``SrcSnkSolver`` expose reusable source-sink solving
  infrastructure.

Typical usage
-------------

.. code-block:: bash

   ./build/bin/lotus-check --engine=saber input.bc
   ./build/bin/lotus-check --engine=saber input.bc --checks=all
   ./build/bin/lotus-check --engine=saber input.bc --checks=double-free,file-leak

Behavior
--------

- With no ``--checks`` option, all Saber checks run.
- When multiple checks are enabled, the tool tries to build and reuse shared
  SVFG and ICFG state across checkers.
- Saber tuning parameters are explicitly namespaced, for example
  ``--saber.context-limit``, ``--saber.max-forward-items``, and
  ``--saber.solver-timeout-ms``.

Interpreting findings
---------------------

Saber reports source-to-sink relationships that satisfy its value-flow model.
Review the diagnostic trace together with the modeled allocation and library
semantics before treating a report as confirmed.  Calls without available
bodies or summaries can affect precision, so use the shared annotation and
alias configuration consistently when comparing runs or triaging results.

Scope and alternatives
----------------------

Saber currently implements resource-management checks only: memory leaks,
double frees, and file-descriptor leaks.  In particular, it is not a general
use-after-free or null-dereference checker.  Use ``ae``, ``pulse``, ``fitx``,
or ``symex`` for the latter memory-safety classes.  See
:ref:`Choosing a Checker <choosing-a-checker>` for engine selection.

See also
--------

- See :doc:`../tools/checker/index` for the tool overview.
- See :doc:`ae`, :doc:`pulse`, and :doc:`fitx` for other memory-safety checker
  families.
