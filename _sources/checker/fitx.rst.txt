FiTx: Daily Development-friendly Bug Detection
==============================================

FiTx is a framework for generating **daily development-friendly bug checkers** that focus on well-known bug patterns. It is based on the approach described in:

  **"Balancing Analysis Time and Bug Detection: Daily Development-friendly Bug Detection in Linux"**  
  Keita Suzuki, Kenta Ishiguro, Kenji Kono.  
  USENIX Annual Technical Conference (ATC), 2024.  
  https://www.usenix.org/conference/atc24/presentation/suzuki

**Library Location**: ``lib/Checker/FiTx/``

**Headers**: ``include/Checker/FiTx/``

**Tool Location**: ``tools/checker/lotus-check-fitx.cpp``

Overview
--------

FiTx balances the trade-off between **analysis time** and **bug detection**: it uses computationally lightweight analyses and limits scope to a single compilation unit (translation unit), so that checkers can run quickly during daily development while still finding many real bugs.

The engine-specific ``--fitx.report-all-candidates`` parameter disables the
normal candidate filtering. Shared selection and report controls remain under
``--checks`` and the common report options.

Design principles from the paper:

* **Short analysis time**: Path-insensitive, inter-procedural analysis on the CFG; bottom-up summary-based interprocedural analysis; no indirect function calls; static field offsets only (struct fields or constant array indices).
* **Targeted bug detection**: Typestate-based analysis for well-known patterns (double free, use-after-free, memory leak, refcount errors, double lock/unlock, etc.) that are findable with these lightweight techniques ("FiT analysis" / finger-traceable analysis).

FiTx is not intended to replace heavy-weight static analyzers; it acts as an **early warning** during build/integration so that developers get fast feedback, and more sophisticated tools can focus on harder bugs later.

Analysis Approach (from the paper)
----------------------------------

**FiT analysis** (finger traceable):

* Scope: single compilation unit.
* Dataflow: inter-procedural, path-**insensitive**, field-sensitive.
* Alias: intra-procedural only; no interprocedural alias.
* Control flow: direct calls only (no indirect calls).
* Field offsets: compile-time only (struct fields, constant array indices).

**Typestate analysis**:

* Each bug is specified as a **typestate property**: a finite state machine (FSM) over variables.
* **Transition rules** consist of: hook instruction (e.g. call, store, load), operand constraints, and target operand. See paper Table 5 (Fun Arg, Fun Call, Store ANY/NULL/NON/Const, Use).
* The analysis traverses the CFG, merges states from predecessors (by priority, e.g. distance to bug state to suppress false positives), and applies transitions at each instruction.

**Return-code aware state propagation** (Section 4.3 of the paper):

* Function summaries record states per **return code** (e.g. 0 for success, negative for error).
* At a call site, if the return value is used in a branch (e.g. ``if (err)``), only the states for the return codes that satisfy that branch are propagated. This reduces false positives from mixing success and error paths.

**Bug-checking hooks** (paper Table 6):

* ``IMMEDIATE``: right after a transition (default).
* ``VAR_END``: end of variable lifetime.
* ``BLOCK_END``: end of basic block.
* ``FUNC_END``: end of function.
* ``MODULE_END``: end of analysis (e.g. for functions with no in-unit callers).

Components
----------

**Frontend** (``lib/Checker/FiTx/Frontend/``):

* ``Framework.cpp`` / ``Framework.h`` – Main FiTx pass; defines typestate checkers and runs them (paper Section 4, 5).
* ``Analyzer.cpp`` – CFG-based typestate analysis: block-by-block state merge, transition application, return-code aware propagation from callee summaries (paper Section 4.2, 4.3).
* ``State.h``, ``StateTransition.h`` – Typestate FSM: states, transitions, notification timing (IMMEDIATE, FUNCTION_END, END_OF_LIFE, MODULE_END).

**Framework IR** (``lib/Checker/FiTx/Framework_IR/``):

* ``Analyzer.cpp`` – Builds the framework IR from LLVM: calls, stores, loads, returns; collects possible return values per basic block for **function summaries** and return-code aware propagation (paper Section 4.3).

**Core** (``lib/Checker/FiTx/Core/``):

* ``BasicBlock.h``, ``Function.h``, ``Value.h`` – CFG and value representation with field-sensitive structure (static field offsets).
* ``Instructions/BranchInstruction.cpp`` – Branch/switch conditions; used to resolve **return-code** at call sites (e.g. ``if (err)`` vs ``if (!err)``) for propagating the correct callee summary (paper Section 4.3).
* ``ValueTypeAlias.cpp`` – Intra-procedural alias for store/load (paper: intra-procedural alias only).

**Detectors** (``lib/Checker/FiTx/Detector/``):

* Each detector (e.g. ``DF_detector/``, ``UAF_detector/``, ``Leak_detector/``, ``Ref_count_detector/``) defines a typestate FSM for one bug pattern (paper Section 4.1, Table 5; Section 5: DF, DL/DUL, ML, UAF, Ref).

Bug Types
--------

FiTx targets well-known patterns that are FiT-analysis findable (paper Table 2, Section 5):

* **Double Free (DF)** – Same memory freed twice.
* **Use After Free (UAF)** – Use of freed memory.
* **Memory Leak (ML)** – Allocated memory not freed.
* **Reference counting errors (Ref)** – Missing get/put or unbalanced refcount.
* **Double Lock / Double Unlock (DL / DUL)** – Lock/unlock API misuse.
* **Null Pointer Dereference** – Use of a value tracked as null.
* **Use Before Initialization (UBI)** – Use of an uninitialized value.

The bundled aggregate detector includes the null-pointer and UBI typestate
definitions in addition to the resource and locking checks above.  The
framework is extensible with new typestate definitions; this does not imply
that every FiT-findable pattern described by the paper is enabled today.

Scope and alternatives
----------------------

FiTx is intended for fast, translation-unit-oriented development feedback.  It
overlaps with other engines by design: use ``pulse`` for bounded,
witness-oriented memory-safety diagnosis, ``ae`` for a broad
abstract-execution pass, and ``saber`` for sparse value-flow leak and
double-free checks.  See :ref:`Choosing a Checker <choosing-a-checker>` for the
bug-class guide. Use ``--checks=<id[,id...]>`` to select ``double-free``,
``double-lock``, ``double-unlock``, ``memory-leak``, ``null-deref``,
``use-after-free``, ``use-before-init``, ``ref-count``, or ``ref-uncount``.
Omitting the option, or passing ``--checks=all``, enables the full set.

Understanding the code
----------------------

**Analysis flow**

1. **Framework IR** (``Framework_IR/Analyzer.cpp``): For each LLVM function, build a framework CFG (basic blocks, instructions as Call/Store/Load/Return). Collect possible return values per block (constants, call results) for building function summaries.

2. **Main pass** (``frontend/Framework.cpp``): For each typestate checker (StateManager), create an Analyzer and run ``analyze()``. The analyzer iterates over all framework functions in the module.

3. **Per-function analysis** (``frontend/Analyzer.cpp``, ``analyzeFunction``): Worklist over basic blocks. For each block: (a) ``createBasicBlockInfo`` merges states from all predecessors; (b) ``analyzePrevBlockBranch`` applies branch-dependent transitions (e.g. ptr==NULL on true path); (c) for each Call/Store/Load, apply the matching typestate transitions (Fun Arg, Store, Use, Alias); (d) if any state changed, re-queue the block; (e) at end of block, run bug-checking hooks (IMMEDIATE, END_OF_LIFE). After the worklist, ``analyzeReturnValue`` builds the function summary (return code → blocks), then report at FUNCTION_END and MODULE_END if applicable.

4. **Call handling**: On a call, first apply **Fun Arg** transitions (e.g. kfree(ptr)). If the callee is defined in this TU, analyze it (bottom-up), then **copyFunctionValues** or **addPendingFunctionValues**: if the return value is used in a branch (e.g. ``if (err)``), only the callee states for the return codes that satisfy that branch are propagated (**return-code aware propagation**); otherwise merge states from all callee exit blocks.

**Key data structures**

* **FunctionInformation** (``frontend/Function.h``): Per-function summary: ``basic_block_info_`` (per-block value states), ``return_info_`` (return code → set of blocks), ``value_collection_`` (related/parent values for propagation).

* **BasicBlockInformation** (``frontend/BasicBlock.h``): Per-block state: ``value_states_`` (value → TransitionLogs), ``arg_value_states_`` (for summary: arg index → (value, transitions)), ``pending_values_`` (per successor block: pending arg states and return value for return-code aware propagation).

* **TransitionLogs** (``frontend/State.h``): History of (transition, instruction) for one value; ``LeastSignificantSource`` is used to filter reports (only report if the value reached the bug state from a “real” init).

* **StateTransitionManager** (``frontend/State.h``): Lookup transitions by trigger: Fun Arg (function name + arg index), Store (NULL/NON/ANY/CALL_FUNC), Use, Alias. Each detector registers its typestate transitions here.

**Adding a new detector**

1. Define states (init, intermediate, bug) and transitions in a ``define_states(StateManager&)`` (e.g. ``DFUtils.cpp``).
2. Register Fun Arg / Store / Use / Alias transitions via ``TransitionManager``.
3. Create a ``FrameworkPass`` subclass that overrides ``defineStates()`` and calls your ``define_states``; add it to ``FrameworkPass::passes``.

References
----------

* Suzuki, K., Ishiguro, K., Kono, K. "Balancing Analysis Time and Bug Detection: Daily Development-friendly Bug Detection in Linux." USENIX ATC 2024.
* FiTx prototype: https://github.com/sslab-keio/FiTx

See Also
--------

* :doc:`index` – Checker framework overview
* :doc:`taint` – configurable IFDS source-to-sink analysis
