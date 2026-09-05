Pulse Checker
=============

Pulse is Lotus's witness-oriented bug finder. It is inspired by Infer Pulse and
uses biabductive analysis with incorrectness-logic semantics: it under-
approximates program behavior to report bugs that should correspond to a
feasible execution, rather than trying to prove all executions safe.

**Library Location**: ``lib/Checker/Pulse/``

**Headers**: ``include/Checker/Pulse/``

**Tool Location**: ``tools/checker/lotus-check-pulse.cpp``

**Frontend**: ``lotus-check --engine=pulse``

Overview
--------

Pulse combines several ideas:

* **Incorrectness logic**: favor witnessable bugs over whole-program safety
  proofs.
* **Biabduction**: track both the current post-state and the missing
  preconditions required to make a witness feasible.
* **Path sensitivity**: keep separate states for feasible branches, subject to a
  bounded disjunct limit.
* **Interprocedural summaries**: summarize callees as pre/post pairs and
  materialize inferred preconditions at call sites.

At a high level the checker performs bounded symbolic execution over LLVM IR,
records concrete error traces, and flushes deduplicated diagnostics through the
shared ``BugReportMgr`` backend.

Core Representation
-------------------

``PulseChecker`` coordinates the analysis:

* Builds an SCC-aware function schedule for summary construction
* Traverses CFGs with bounded disjunction and loop abstraction
* Dispatches instructions to transfer functions and library models
* Converts detected issues into diagnostics and bug reports

The core abstract state is ``AbductiveDomain``:

* **Post-state**: heap, stack, address attributes, taint, and path facts along
  the current witness path
* **Pre-state**: facts that had to be abduced to justify a later dereference or
  access
* **Path formula**: equalities, nullness facts, disequalities, and arithmetic
  constraints used to prune infeasible paths

Runtime progress is represented by ``ExecutionDomain``:

* ``ContinueProgram`` for normal symbolic execution
* Stopped variants such as ``ExitProgram``, ``AbortProgram``, and latent abort
  states when the checker needs caller context before turning an issue into a
  report

Key Components
--------------

The Pulse tree is organized by responsibility:

* ``Checker/``: main checker driver, instruction semantics, and summary wiring
* ``Core/``: formulas, call state, substitution, memory model, and value
  history
* ``Domain/``: abductive state, joins, disjunctive and non-disjunctive domains,
  loop abstraction, taint, and operations
* ``Interproc/``: summaries, specialization, transitive info, and library
  models
* ``Report/``: diagnostics, latent issue handling, logging, and report flushing

Not every concept has a dedicated ``.cpp`` file. For example, ``PulseMemory``,
``PulseValueHistory``, and ``PulseInvalidation`` are currently header-defined
types used by the implementation rather than standalone translation units.

Detected Bug Classes
--------------------

Pulse currently registers and reports these issue types:

* **Use After Free**
* **Null Pointer Dereference**
* **Uninitialized Read**
* **Taint Error**
* **Out Of Bounds Access**
* **Invalid Free**
* **Stack Variable Address Escape**
* **Unnecessary Copy**

The checker also tracks ``Const-Refable Parameter`` information, but that path
is not yet emitted as a first-class ``PulseDiagnostic``.

Interprocedural Behavior
------------------------

Pulse summarizes functions as disjunctive pre/post pairs:

* Each summary entry records an inferred precondition, a post-state, path
  formulas, an optional return value, and optional latent issue information.
* Call-site application materializes the callee's inferred precondition into the
  caller state.
* Calls inside the current SCC are treated conservatively while summaries are
  still unstable.
* Unknown or rejected summaries are tracked so the caller can fall back to a
  conservative unknown-result path instead of fabricating a witness.

Loops and Path Explosion
------------------------

Pulse is intentionally bounded:

* ``kMaxDisjuncts = 10`` limits how many disjunctive states are retained
* ``kMaxCallDepth = 5`` limits recursive interprocedural exploration
* ``LoopAbstraction`` applies widening and path-stamp based convergence checks
* ``--pulse.smt=off`` disables SMT-backed path pruning for a faster, less precise mode

These limits trade recall for scalability. In practice, they can introduce
false negatives by pruning or merging feasible witnesses too aggressively.

Command-Line Usage
------------------

Basic usage:

.. code-block:: bash

   ./build/bin/lotus-check --engine=pulse input.bc

Common options:

* ``--verbose``: print detailed finding traces
* ``--log-level=<level>``: ``none``, ``error``, ``warning``, ``info``,
  ``debug``, or ``trace``
* ``--analysis-stats``: print the shared checker analysis summary
* ``--report-json=<path>``: emit the shared JSON report format
* ``--report-min-score=<0-100>``: filter exported reports by confidence
* ``--pulse.smt=off``: disable SMT solving when checking path feasibility

Example:

.. code-block:: bash

   ./build/bin/lotus-check --engine=pulse input.bc --log-level=debug --report-json=report.json

Programmatic Usage
------------------

.. code-block:: cpp

   #include "Alias/Infrastructure/AliasAnalysisWrapper/AliasAnalysisWrapper.h"
   #include "Checker/Pulse/Checker/PulseChecker.h"
   #include "Checker/Framework/BugReportMgr.h"

   auto AA = std::make_unique<lotus::AliasAnalysisWrapper>(
       *M, lotus::AAConfig::UnderApprox());

   pulse::PulseChecker checker(M.get(), AA.get());
   checker.analyze();

   BugReportMgr &mgr = BugReportMgr::get_instance();
   mgr.deduplicate_reports(BugReportMgr::DedupMode::ExactTrace);
   mgr.print_summary(llvm::outs());

Testing
-------

The main unit tests live in:

* ``tests/unit/Checker/Pulse/PulseCheckerTest.cpp`` for end-to-end checker
  behavior and reporting
* ``tests/unit/Checker/Pulse/PulseFormulaTest.cpp`` for formula-level reasoning

Run the relevant tests from the build directory with:

.. code-block:: bash

   ctest --output-on-failure -R 'pulse_checker_test|PulseFormulaTest'

Limitations
-----------

Pulse is designed to avoid non-witnessable reports on the behaviors it models,
but precision still depends on the quality of its summaries, path reasoning, and
library models.

Current practical limitations include:

* bounded disjunction and call depth
* incomplete library modeling
* summary rejection when caller facts do not satisfy a callee witness
* loss of precision in complex loops and merged control flow

Scope and alternatives
----------------------

Pulse is the witness-oriented choice for the memory-safety classes listed
above.  It is bounded and therefore is not a replacement for every memory
checker: use ``ae`` for a broad abstract-execution pass, ``fitx`` for fast
translation-unit feedback, ``saber`` for leak and double-free value-flow
checks, and ``symex`` when SMT-backed symbolic reasoning is required.  See
:ref:`Choosing a Checker <choosing-a-checker>` for the full mapping.

See Also
--------

* :doc:`index` for the checker framework overview
* :doc:`../alias/index` for the alias analyses Pulse can use
