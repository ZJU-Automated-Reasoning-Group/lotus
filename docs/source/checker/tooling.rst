Checker Tooling
===============

Subcommand registration and entry points for the ``lotus-check`` unified
checker frontend.

**Header**: ``include/Checker/Framework/Subcommands.h``

**Implementation**: ``tools/checker/``

**Build target**: ``lotus-check``

Overview
--------

The Checker Tooling module provides the subcommand infrastructure for the
unified ``lotus-check`` binary. Each checker category registers an
``llvm::cl::SubCommand`` with a descriptive name and help text, enabling
dispatch from a single entry point.

.. note::
   These subcommands are an internal C++ option-registration mechanism. The
   public command-line interface selects an engine with ``--engine=<name>``
   (e.g. ``lotus-check --engine=kint input.bc``); the old ``lotus-check kint
   input.bc`` subcommand form is rejected.

Checker Subcommands
-------------------

**File**: ``Subcommands.h``

Declares accessor functions for each subcommand (defined in
``lib/Checker/Framework/Subcommands.cpp``):

.. code-block:: cpp

   #include "Checker/Framework/Subcommands.h"

   auto &sub = lotus::checker::tooling::genericSubCommand();
   auto &sub = lotus::checker::tooling::kintSubCommand();
   auto &sub = lotus::checker::tooling::taintSubCommand();
   auto &sub = lotus::checker::tooling::concurrencySubCommand();
   auto &sub = lotus::checker::tooling::pulseSubCommand();
   auto &sub = lotus::checker::tooling::fitxSubCommand();
   auto &sub = lotus::checker::tooling::saberSubCommand();
   auto &sub = lotus::checker::tooling::aeSubCommand();
   auto &sub = lotus::checker::tooling::symexSubCommand();

Registered Subcommands
----------------------

+-------------------+-------------------------------------------+
| Subcommand        | Engine selected via ``--engine=<name>``   |
+===================+===========================================+
| ``generic``       | Run declarative and registry-backed       |
|                   | checks                                    |
+-------------------+-------------------------------------------+
| ``kint``          | Run the KINT integer checker              |
+-------------------+-------------------------------------------+
| ``taint``         | Run the IFDS-based taint analysis         |
+-------------------+-------------------------------------------+
| ``concur``        | Run the concurrency checker suite         |
+-------------------+-------------------------------------------+
| ``pulse``         | Run the Pulse checker                     |
+-------------------+-------------------------------------------+
| ``fitx``          | Run the FiTx checker suite                |
+-------------------+-------------------------------------------+
| ``saber``         | Run the Saber checker                     |
+-------------------+-------------------------------------------+
| ``ae``            | Run the abstract-execution checker        |
+-------------------+-------------------------------------------+
| ``symex``         | Run the symbolic-execution checker        |
+-------------------+-------------------------------------------+

Usage
-----

.. code-block:: bash

   ./build/bin/lotus-check --engine=kint input.bc --checks=all
   ./build/bin/lotus-check --engine=concur input.bc --checks=data-race

Parameter namespaces
--------------------

Options with one cross-engine contract remain global, for example
``--checks``, ``--log-level``, ``--analysis-stats``, and the shared reporting
options. Parameters whose semantics belong to an engine use the qualified form
``--<engine>.<parameter>``. For example, KINT summary behavior is configured by
``--kint.summary-mode``, while Pulse path solving is configured by
``--pulse.smt``. No unqualified compatibility aliases are provided.

Use ``lotus-check --list-parameters`` to print all global and engine groups,
or ``lotus-check --engine=<name> --help`` for one engine.

See Also
--------

- :doc:`./index` — Checker framework overview
- :doc:`./kint` — KINT integer checker
- :doc:`./pulse` — Pulse biabductive checker
- :doc:`./saber` — Saber source-sink checker
- :doc:`./symex` — symbolic-execution checker
- :doc:`./taint` — IFDS taint checker
