Sifa Tool
=========

This page documents the ``lotus-verify-sifa`` verifier front-end in ``tools/verifier/sifa/``.

**Binary**: ``lotus-verify-sifa``

**Source**: ``tools/verifier/sifa/lotus-verify-sifa.cpp``

Overview
--------

``lotus-verify-sifa`` analyzes a target function or block using the Sifa framework. It can
run in a lightweight reachability mode, a native abstract-domain mode, or a
SymAbs-backed SMT mode.

Basic usage
-----------

.. code-block:: bash

   ./build/bin/lotus-verify-sifa input.bc
   ./build/bin/lotus-verify-sifa input.bc --function=foo
   ./build/bin/lotus-verify-sifa input.bc --function=foo --block=loop.header
   ./build/bin/lotus-verify-sifa input.bc --symabs --abstract-domain=Octagon

Important options
-----------------

- ``--function=<name>`` selects the function to analyze. The default is
  ``main`` when available.
- ``--block=<label>`` analyzes to a named block instead of the function return.
- ``--abstract-domain=Interval|Octagon|both`` selects the abstract domain.
- ``--symabs`` enables SMT-backed symbolic abstraction.
- ``--represent-all`` includes unnamed SSA values in the printed invariant.
- ``--widening-delay`` and ``--widening-frequency`` tune widening in SymAbs
  mode.
- ``--reachability`` only reports whether the target is reachable.
- ``--list-functions`` and ``--list-blocks`` inspect available analysis targets.
- ``--log-level=none|error|warning|info|progress|debug|trace`` controls
  diagnostic output.

See also
--------

- See :doc:`../../../verification/sifa` for the framework-level overview.
