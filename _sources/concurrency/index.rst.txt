Concurrency Analysis
====================

This section documents the concurrency analysis subsystem, the shared
thread-aware analysis layer used by Lotus concurrency checking and by clients
that need May-Happen-in-Parallel (MHP), lock-set, or happens-before reasoning.

**Headers**: ``include/Concurrency/``

**Implementation**: ``lib/Concurrency/``

Overview
--------

The concurrency analysis module provides the shared analysis layer used by Lotus
concurrency checking. It centers on thread creation, synchronization operations,
memory accesses, and communication structure that can be recovered from LLVM IR.

The module supports multiple concurrency models:

- **POSIX threads (pthreads)**: standard pthread APIs
- **Modern C++ concurrency**: ``std::thread``, mutexes, condition variables,
  futures/promises, atomics, and several C++20 primitives that are recognized
- **OpenMP**: parallel regions, barriers, tasks, and lock operations
  (see ``lib/Concurrency/OpenMP/``)
- **MPI**: a separate SPMD communication analysis in ``Concurrency/MPI/``
- **CUDA**: thread/block hierarchy and PTX-level reasoning
  (see ``lib/Concurrency/CUDA/``, including ``PTXAnalyzer``)
- **Linux kernel**: kernel concurrency primitives
  (see ``lib/Concurrency/LinuxKernel/``)
- **Custom APIs**: configurable through ``config/thread.spec`` and ``ThreadAPI``

**Key capabilities**:

- May-Happen-in-Parallel (MHP) analysis for shared-memory overlap reasoning
- Lock-set analysis for race and deadlock checking
- Happens-before computation from thread-flow and synchronizes-with edges
- Escape and thread-sharing analyses for shared-memory filtering
- Join-target reasoning for refining ``pthread_join`` effects
- Thread flow graph construction and thread API classification utilities

Directory layout
----------------

The source tree is grouped by subdirectory under ``include/Concurrency/`` and
``lib/Concurrency/``:

- ``Utils/``: ``ThreadAPI``, ``ThreadFlowGraph``, vector-clock utilities,
  RAII lock tracking, and language models for C++, OpenMP, MPI, and Linux kernel
  APIs
- ``MHP/``: ``MHPAnalysis``, ``StaticVectorClockMHP``, and
  ``HappensBeforeAnalysis``
- ``LockSet/``: ``LockSetAnalysis``
- ``Memory/``: ``EscapeAnalysis`` and ``StaticThreadSharingAnalysis``
- ``JoinTarget/``: ``JoinTargetAnalysis``
- ``MPI/``: ``MPIAnalysis`` and its process, collective, rank, and RMA analyses
- ``CUDA/``: ``CUDAAnalysis``, ``CUDAFunctionSummary``, ``CUDASemantics``, and
  ``PTXAnalyzer``
- ``OpenMP/``: ``OpenMPModel``, ``OpenMPSemantics``, and ``OpenMPTaskGraph``
- ``LinuxKernel/``: ``LinuxKernelAnalysis``, ``LinuxKernelLockAnalysis``, and
  ``LinuxKernelRCUAnalysis``

``Concurrency/MPI/`` models MPI communication in the SPMD setting. It complements
the shared-memory analyses above, but it is not another thread library layered on
top of ``MHPAnalysis``.

Clients
-------

The concurrency analyses feed two main consumers:

- The **concurrency bug checkers** (data races, deadlocks, atomicity violations,
  OpenMP and MPI bugs) exposed through ``lotus-check --engine=concur``. See
  :doc:`../checker/concurrency` for the checker side.
- Any analysis client that needs MHP, lock-set, or happens-before queries via
  ``ConcurrencyFacade``.

.. toctree::
   :maxdepth: 2

   analysis

See Also
--------

* :doc:`../checker/concurrency` – Concurrency bug checkers (``lotus-check --engine=concur``)
* :doc:`../analysis/index` – Other analysis utilities
