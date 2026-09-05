Command-Line Tools
==================

Lotus provides command-line tools for alias analysis, CFL reachability, bug
detection, IR querying, optimization, verification, and solver experiments.

This section focuses on the front-end binaries and tool families under
``tools/``. The documentation is organized by subdirectory to match the source
structure, but some pages also note when a tool family contains source-present
experiments that are not built by default.

For a feature-oriented walk-through, see :doc:`../user_guide/tutorials` and
:doc:`../user_guide/bug_detection`.

Tools by Subdirectory
---------------------

The documentation is organized to match the ``tools/`` directory structure:

.. toctree::
   :maxdepth: 2

   alias/index
   cfl/index
   checker/index
   dataflow/index
   ir/index
   optimization/index
   solver/index
   verifier/index

Scripting Tools
---------------

.. toctree::
   :maxdepth: 1

   phoenix
