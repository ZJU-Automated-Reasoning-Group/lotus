Dataflow Tools
==============

This page documents the testing and comparison tools under ``tools/dataflow/``.
They are primarily intended for validating and comparing Lotus dataflow engines
over the same LLVM IR input.

Overview
--------

The current front-ends focus on intraprocedural or IFDS-style benchmark runs and
emit machine-readable summaries that are easy to diff in tests.

lotus-dfa
---------

Differential-testing front-end that compares multiple engines on the same
analysis problem.

**Binary**: ``lotus-dfa``

**Source**: ``tools/dataflow/lotus-dfa-diff.cpp``

**Usage**:

.. code-block:: bash

   ./build/bin/lotus-dfa --analysis=liveness input.bc
   ./build/bin/lotus-dfa --analysis=constant_prop --engine=all input.bc
   ./build/bin/lotus-dfa --analysis=reaching_defs --engine=ifds input.bc

Important options:

- ``--analysis=liveness|reaching_defs|uninitialized|constant_prop|available_exprs|reachable``
- ``--engine=elim|mono|ifds|all``
- ``--elim-method=state|adt-simple|adt-delayed``
- ``--out-dir=<dir>``

lotus-dfa-apa
-------------

Standalone front-end for the elimination-based APA engine.

**Binary**: ``lotus-dfa-apa``

**Source**: ``tools/dataflow/lotus-dfa-apa.cpp``

Supports the same ``--analysis`` space as ``lotus-dfa`` plus
``--elim-method`` for choosing the elimination solver variant.

Profiling-oriented dumps are available with:

- ``--dump-profile`` to emit CFG, solver, and path-expression summary metrics
- ``--dump-exprs`` to additionally emit per-instruction path-expression stats and serialized expressions

lotus-dfa-mono
--------------

Standalone front-end for the Mono engine.

**Binary**: ``lotus-dfa-mono``

**Source**: ``tools/dataflow/lotus-dfa-mono.cpp``

Supported analyses:

- ``liveness``
- ``reachable``
- ``constant_prop``
- ``uninitialized``

lotus-dfa-ifds
--------------

Standalone front-end for IFDS-based analyses.

**Binary**: ``lotus-dfa-ifds``

**Source**: ``tools/dataflow/lotus-dfa-ifds.cpp``

Supported analyses:

- ``reaching_defs``
- ``uninitialized``

lotus-dfa-npa
--------------

Standalone front-end for the NPA (Newton Program Analysis) engine supporting
both intraprocedural and interprocedural analyses.

**Binary**: ``lotus-dfa-npa``

**Source**: ``tools/dataflow/lotus-dfa-npa.cpp``

**Usage**:

.. code-block:: bash

   lotus-dfa-npa [options] <bitcode file>

**Options**:

- ``<bitcode>``: positional, required, path to LLVM .bc or .ll file
- ``--out-dir <dir>``: string, default "", output directory (writes npa.txt)
- ``--stdout``: bool, default false, force output to terminal
- ``--analysis <name>``: string, default "liveness", one of: liveness, reaching_defs, reachable, inter_liveness, inter_reaching_defs, inter_uninitialized, inter_constant_prop, inter_interval, inter_nullability
- ``--solver <name>``: string, default "newton", newton or kleene (inter analyses require newton)
- ``--linear-solver <name>``: string, default "scc", scc, adaptive_scc, or tensor
- ``-nworkers=<N>``: unsigned, parallel workers for function scheduling

Intraprocedural analyses:

- ``liveness``
- ``reaching_defs``
- ``reachable``

Interprocedural analyses:

- ``inter_liveness``
- ``inter_reaching_defs``
- ``inter_uninitialized``
- ``inter_constant_prop``
- ``inter_interval`` (range analysis)
- ``inter_nullability`` (null-pointer analysis)

Output format:

.. code-block:: none

   [npa:<analysis>:<scope>:linear=<linear-solver>:workers=<N>:parallel=<on|off>]
   FUNC <name>
     bb<N> IN: <comma-separated value ids>

See also
--------

- See :doc:`../../dataflow/apa`, :doc:`../../dataflow/mono`,
  :doc:`../../dataflow/ifds_ide`, and :doc:`../../dataflow/npa` for the
  underlying engines.
