Sifa
====

Sifa stands for Symbolic Interpretation with Fluid Abstractions.

**Headers**: ``include/Verification/Sifa/``

**Implementation**: ``lib/Verification/Sifa/``

**Tool**: ``lotus-verify-sifa`` in ``tools/verifier/sifa/lotus-verify-sifa.cpp``

Overview
--------

Sifa performs symbolic interpretation over procedure graphs and regex-style DAG
summaries, with configurable abstract domains and "fluid" policies that control
when and how abstraction is applied.

Main components
---------------

- ``Procedure/`` builds procedure graphs and associated resources.
- ``Cfg/Transition`` models the transitions Sifa reasons about.
- ``RegexDag/`` contains the regex-to-DAG and compression infrastructure used
  for summary construction.
- ``Summarizers/`` computes loop and procedure summaries.
- ``Domain/`` provides abstract domains such as reachability, intervals,
  octagons, equalities, explicit values, and compound domains.
- ``Fluid/`` defines policies for controlling abstraction aggressiveness.
- ``Worklist/`` and ``Caches/`` support scalable summary computation.

Domain choices
--------------

The in-tree front-end exposes interval and octagon domains directly, and can
also delegate to SymAbs-backed symbolic abstraction.

Typical usage
-------------

.. code-block:: bash

   ./build/bin/lotus-verify-sifa program.bc
   ./build/bin/lotus-verify-sifa program.bc --function=foo --block=loop.header
   ./build/bin/lotus-verify-sifa program.bc --symabs --abstract-domain=Octagon
   ./build/bin/lotus-verify-sifa program.bc --reachability

Selecting an abstraction
------------------------

Intervals are a lightweight choice for single-variable bounds, while octagons
can retain relations expressible as sums and differences of two variables.
More expressive domains can improve a proof but also enlarge summaries and
increase solving time.  Start with a small domain and a specific function or
block when investigating a result, then broaden the scope once the modeled
transitions and abstraction policy have been checked.

See also
--------

- See :doc:`symabs-ai` for the SymAbs integration path.
- See :doc:`../tools/verifier/sifa/index` for the CLI-focused page.
