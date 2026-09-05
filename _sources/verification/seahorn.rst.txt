SeaHorn Verification Framework
==============================

Large-scale SMT-based verification framework built on constrained Horn clauses
(CHC), symbolic execution, and abstraction-refinement.

Overview
--------

SeaHorn performs bounded and unbounded model checking on LLVM bitcode using SMT
solvers to prove program correctness or produce counterexamples. It integrates
with Lotus analyses and solver backends to support complex verification
pipelines.

**Vendored source**: ``third-party/verification/seahorn/``

**Lotus adapter**: ``lib/Verification/Backends/``

**Build Targets**:

- ``tools/verifier/seahorn/seahorn``
- ``tools/verifier/seahorn/seapp``
- ``tools/verifier/seahorn/seainspect``
- ``third-party/horn-ice/chc_verifier``
- ``third-party/horn-ice/hice-dt``

Structure
---------

The SeaHorn framework is organized into the following subdirectories:

* **seahorn/** – Core SeaHorn verification engine
  * Horn clause generation and solving (HornifyFunction, HornifyModule)
  * Symbolic execution engines (BvOpSem, ClpOpSem, UfoOpSem)
  * Bounded model checking (Bmc, PathBmc)
  * **K-Induction** – Production k-induction engine reusing PathBMC (in ``KInduction/``)
  * Counterexample generation (CexHarness, CexExeGenerator)
  * **Smt/** – SMT solver integration (Z3, Yices2)

* **Analysis/** – Analysis passes for verification
  * ``CrabAnalysis.cc`` – CRAB abstract interpretation integration
  * ``CanAccessMemory.cc`` – Memory access analysis
  * ``CanFail.cc`` – Failure point analysis
  * ``CanReadUndef.cc`` – Undefined value read analysis
  * ``ClassHierarchyAnalysis.cc`` – C++ class hierarchy analysis
  * ``ControlDependenceAnalysis.cc`` – Control dependence computation
  * ``CutPointGraph.cc`` – Cut point graph construction
  * ``GateAnalysis.cc`` – Gate analysis for verification
  * ``StaticTaint.cc`` – Static taint analysis
  * ``TopologicalOrder.cc``, ``WeakTopologicalOrder.cc`` – Ordering analyses

* **Transforms/Kernel/** – DrvHorn: Kernel Driver Verification

  DrvHorn is a set of LLVM IR transformation passes enabling SeaHorn to verify
  reference-count correctness (kref/kobject) in Linux kernel drivers. The passes
  run as preprocessing before Horn clause generation, transforming kernel driver
  LLVM IR into a verification-friendly form.

  **Pipeline order**: KernelSetup → InitGlobalKrefs → Device/Devm/Platform/
  I2CDriver/FileOperations/DsaSwitchOps (modeling) → HandleInlineAsm →
  AssumeNonNull → NondetMalloc → Slicer → IntoBinary → AssertKrefs

  **KernelSetup** (*KernelSetup.cc*)
    Foundational kernel environment setup. Stubs ~13 allocation functions
    (kmalloc, vmalloc, etc.) through drvhorn.alloc. Builds replacements for
    kref_init/get/put and kobject_get/put. Handles kmem_cache, IS_ERR/ERR_PTR
    patterns, and stubs __cpu_possible_mask.

  **InitGlobalKrefs** (*InitGlobalKrefs.cc*)
    Initializes all global kref instances. Scans global variables, creates
    drvhorn.kref.* snapshot globals, and inserts a drvhorn.prelude function for
    initialization.

  **Device** (*Device.cc*)
    Central kernel device lifecycle modeling (~1741 lines). Models device finding
    (class_find_device, bus_find_device), storage-backed allocation,
    container_of pattern handling, device initialization (kref, release
    functions), OF/fwnode management (~40 functions), device links, wakeup, devm
    functions, CPU freq, regulator, and LED registration.

  **Devm** (*Devm.cc*)
    Managed device resource modeling. Replaces devres_add by immediately calling
    the release function with a nondeterministically allocated resource.

  **Acpi** (*Acpi.cc*)
    ACPI driver modeling. Replaces the placeholder acpi_driver.add with the
    actual driver callback from the acpi_driver global initializer.

  **Platform** (*Platform.cc*)
    Platform driver entry point generation. Creates main() that allocates a
    platform_device, initializes embedded device (kref, wakeup, driver_data,
    of_node), and calls probe.

  **I2CDriver** (*I2CDriver.cc*)
    I2C driver entry point generation. Creates main() that allocates an
    i2c_client and calls the I2C probe function.

  **FileOperations** (*FileOperations.cc*)
    File operations driver entry point generation. Creates main() that allocates
    inode + file structs, initializes krefs in private data, and calls .open
    callback.

  **DsaSwitchOps** (*DsaSwitchOps.cc*)
    DSA switch driver entry point generation. Creates main() that allocates a
    DSA switch struct and calls the setup callback.

  **SpecificFunction** (*SpecificFunction.cc*)
    Generic fallback entry point generation. Creates main() that calls any named
    function with nondeterministically-initialized arguments.

  **HandleInlineAsm** (*HandleInlineAsm.cc*)
    Inline assembly replacement (~1533 lines). Replaces ~50 categories of Linux
    kernel inline assembly with equivalent LLVM IR: bitops, atomics, barriers,
    MSR, CPUID, TSC, serialization, string ops, control registers, I/O ports,
    get_user, static branches, and more.

  **AssumeNonNull** (*AssumeNonNull.cc*)
    Assumes container_of results are non-null. Detects negative-index GEPs
    (container_of pattern) and inserts verifier assumptions that the pointer is
    not null.

  **NondetMalloc** (*NondetMalloc.cc*)
    Nondeterministic allocation handling. Routes nondet.malloc to either
    drvhorn.malloc (nullable) or plain malloc depending on downstream null
    checks.

  **Slicer** (*Slicer.cc*)
    Driver-specific program slicer. Identifies instructions influencing
    kref/kobject reference counting and removes everything else, replacing
    removed instructions with nondeterministic values.

  **IntoBinary** (*IntoBinary.cc*)
    Boolean simplification. Replaces nondet calls used only in branches with
    booleans. Converts nonzero returns used as conditions to 0/1.

  **AssertKrefs** (*AssertKrefs.cc*)
    Final verification assertion generation. Builds drvhorn.assert_kref (asserts
    refcount==1), drvhorn.assert_wakeup (asserts wakeup==0), storage checker
    functions, and injects assertions at driver fail blocks.

  **Debug** (*Debug.cc*)
    Debugging utilities. Stubs OF functions, prints inline asm statistics, dumps
    IR to file, runs LLVM verifier.

  **ListOps** (*ListOps.cc*)
    Operation discovery. Scans globals matching a struct type and prints their
    names.

  **SetupEntrypoint** (*SetupEntrypoint.hh* / *SetupEntrypoint.cc*)
    Shared entry-point helpers: device pointer setup (kref init, wakeup disable,
    driver_data, of_node), fail block construction, return block construction.

  **Util** (*Util.hh* / *Util.cc*)
    Shared utilities: type equivalence, call extraction, nondet/allocation
    function creation, GEP index computation, struct embedding detection.

  **SlimDown** (*SlimDown.hh*)
    Declared but unimplemented. Intended for module-level pruning by root
    reachability.

  .. note::

     Added in commit 909d6e9c.

* **Transforms/** – LLVM IR transformations for verification
  * **Instrumentation/** – Property instrumentation
    * ``BufferBoundsCheck.cc``, ``FatBufferBoundsCheck.cc`` – Buffer bounds checking
    * ``NullCheck.cc`` – Null pointer checking
    * ``SimpleMemoryCheck.cc`` – Memory safety checks
    * ``MixedSemantics.cc`` – Mixed operational semantics
  * **Scalar/** – Scalar optimizations
    * ``CutLoops.cc`` – Loop cutting
    * ``LoopPeeler.cc`` – Loop peeling
    * ``LowerCstExpr.cc`` – Constant expression lowering
    * ``LowerGvInitializers.cc`` – Global variable initializer lowering
    * ``PromoteVerifierCalls.cc`` – Verifier call promotion
    * ``BackedgeCutter.cc`` – Backedge cutting
  * **Utils/** – Transformation utilities
    * ``DevirtFunctions.cc`` – Function devirtualization
    * ``ExternalizeFunctions.cc`` – Function externalization
    * ``Mem2Reg.cc`` – Memory-to-register promotion
    * ``SliceFunctions.cc`` – Function slicing

* **Support/** – Utility functions
  * ``CFGPrinter.cc`` – Control flow graph printing
  * ``GitSHA1.cc`` – Version information
  * ``Profiler.cc`` – Profiling support
  * ``Stats.cc`` – Statistics collection

Components
----------

- **SeaHorn** – LLVM-based front-end, symbolic execution, and encoding of
  verification problems as CHCs.
- **Horn-ICE** – CHC solving with invariant learning (in ``third-party/horn-ice/``).
- **Sea-rt** – Runtime components for executing counterexample harnesses (in ``tools/verifier/seahorn/sea-rt/``).

SeaHorn Verification
--------------------

Main verification tool for C programs.

**Basic usage**:

.. code-block:: bash

   ./build/bin/seahorn [options] <input.c>

**Common modes**:

- ``--bmc=<N>`` – Bounded model checking up to ``N`` steps.
- ``--horn-kinduction`` – K-induction (PathBMC-based): incrementally peel loops
  and run PathBMC until UNSAT (safe) or SAT (bug). Requires CLAM.
- ``--horn`` – CHC-based (unbounded) verification.
- ``--abstractor=clam`` – Use CLAM-based abstract interpretation as an
  abstractor.

**Frequently used options**:

- ``--cex=<file>`` – Dump a counterexample harness to ``<file>``.
- ``--track=mem`` – Track memory (heap/stack) explicitly.
- ``--horn-solver=spacer|ice`` – Select CHC solving engine.

Counterexample Analysis
~~~~~~~~~~~~~~~~~~~~~~~

Generate executable counterexamples:

.. code-block:: bash

   ./build/bin/seahorn --cex=harness.ll program.c
   clang -m64 -g program.c harness.ll -o counterexample
