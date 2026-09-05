Constant-Time Analysis
======================

Static analysis for verifying constant-time programming properties in
sensitive code.

This page documents the current ``lib/Security/ConstantTime`` subsystem. Older
references to a separate ``Crypto`` analysis directory are stale; the code now
lives under the unified ``lib/Security`` tree.

**Headers**: ``include/Security/ConstantTime``

**Implementation**: ``lib/Security/ConstantTime``

**Build target**: ``ConstantTimeVerify``

Overview
--------

The ConstantTime module hosts Lotus's CT-LLVM-based constant-time analysis for
secret-dependent side channels. The pass tracks tainted values, propagates
taint through data and alias relationships, and reports security-relevant
behaviors such as:

- secret-dependent memory accesses
- secret-dependent branches
- variable-time operations influenced by secrets

This analysis is useful beyond traditional cryptographic code: any sensitive
routine that must avoid timing leakage can be checked with the same framework.

Main Components
---------------

CTPass
~~~~~~

**Files**: ``ConstantTimePass.cpp``, ``ConstantTimePass.h``

``CTPass`` is the public pass entry point for the subsystem.

**Public entry points**:

- ``CTPass``
- pipeline name ``ctllvm``
- plugin registration via ``getPassPluginInfo()`` and
  ``llvmGetPassPluginInfo()``

Core Analysis
~~~~~~~~~~~~~

**File**: ``ConstantTimeAnalysis.cpp``

Implements taint propagation and leak detection inside analyzed code.

**Key responsibilities**:

- identify secret-tainted inputs
- propagate taint through def-use and memory relationships
- classify detected leaks by channel
- accumulate per-function and global analysis results

Inlining and Soundness
~~~~~~~~~~~~~~~~~~~~~~

**File**: ``ConstantTimeInlining.cpp``

Provides recursive inlining support used by the more aggressive analysis mode.
Inlining is used to expose interprocedural flows that would otherwise be hidden
behind call boundaries.

Targets and Reporting
~~~~~~~~~~~~~~~~~~~~~

**Files**:

- ``ConstantTimeTargets.cpp``
- ``ConstantTimeDebugInfo.cpp``
- ``ConstantTimeReporting.cpp``

These files handle source/target selection, debug-info-based lookup, report
generation, statistics, and source-location rendering.

Analysis Modes and Options
--------------------------

The subsystem is now configured through runtime options collected in
``CTOptions`` rather than the older preprocessor-driven setup.

Important defaults include:

- ``type_system = true``
- ``test_all_parameters = true``
- ``enable_may_leak = true``
- ``try_hard_on_name = true``
- ``user_specify = false``
- ``soundness_mode = true``
- ``alias_threshold = 2000``
- ``report_leakages = true``
- ``time_analysis = false``
- ``auto_continue = true``
- ``inline_threshold = 10``
- ``debug = false``
- ``print_function = false``

``file_path`` remains available for source-file lookup during reporting.

Leak Model
----------

The pass reports constant-time violations through several channels:

- **Cache timing**: secret-dependent memory-access behavior
- **Branch timing**: secret-dependent control flow
- **Variable timing**: operations whose latency may vary with secret data

In practice, this means the ConstantTime component now covers analysis that was
historically documented under separate crypto and speculation/security topics,
but is implemented today as one security-oriented constant-time subsystem in
``lib/Security/ConstantTime``.

Usage
-----

The pass can be added to an LLVM pass pipeline:

.. code-block:: cpp

   #include <Security/ConstantTime/ConstantTimePass.h>

   llvm::ModulePassManager MPM;
   MPM.addPass(CTPass());
   MPM.run(M, MAM);

Typical use cases:

- auditing cryptographic implementations
- validating constant-time coding discipline in security-sensitive routines
- integrating side-channel checks into CI
- experimentation with CT-LLVM-style constant-time analysis

Related Components
------------------

- :doc:`spectre` documents the separate ``lib/Security/Spectre`` subsystem for
  Spectre-v1-style speculative cache analysis.
- :doc:`lif` documents ``lib/Security/LIF``, which focuses on isochronous
  transformation and taint-driven control-flow linearization.

Limitations
-----------

- precision depends on taint-source identification and alias information
- may-leak mode can report conservative warnings
- soundness-oriented inlining can fail on unsupported or difficult call
  patterns
- debug information improves source-level reporting quality
