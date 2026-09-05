Spectre Cache Analysis
======================

Cache speculation analysis for detecting Spectre vulnerabilities related to
cache timing side-channels.

This page documents the dedicated ``lib/Security/Spectre`` subsystem inside
Lotus's unified security analysis tree.

**Headers**: ``include/Security/Spectre``

**Implementation**: ``lib/Security/Spectre``

**Build target**: ``Spectre``

Overview
--------

The Spectre module provides Spectre-v1-style speculative cache analysis. It
models architectural and speculative cache state to detect execution patterns
where speculation can expose secret-dependent cache behavior.

Main Components
---------------

CacheModel
~~~~~~~~~~

**Files**: ``CacheModel.cpp`` and ``CacheSpecuAnalysis.h``

Models cache behavior with configurable parameters:

- **Cache line size**: Size of each cache line (default: 16 bytes)
- **Number of cache lines**: Total number of cache lines (default: 32)
- **Number of sets**: Number of cache sets for set-associative caches
- **Associativity**: Cache lines per set

**Features**:

- Tracks abstract memory objects and cache-line occupancy
- Simulates cache hits and misses
- Records speculative versus architectural observations
- Supports merging, widening, and full-cache invalidation helpers

CacheSpecuAnalysis
~~~~~~~~~~~~~~~~~~

**Files**: ``CacheSpecuAnalysis.cpp``, ``CacheSpecuAnalysis.h``

Performs speculative cache analysis to detect potential Spectre vulnerabilities.

**Analysis Flow**:

1. **Initialization**: Builds cache state from globals, stack objects, and
   arguments
2. **Architectural Simulation**: Computes per-block cache states for ordinary
   execution
3. **Speculative Exploration**: Follows branch-dependent speculative windows
4. **Observation Comparison**: Compares speculative and architectural cache
   observations
5. **Finding Construction**: Emits ``SpectreFinding`` records for suspicious
   divergences

**Key Methods**:

- ``InitModel()``: Initializes the abstract cache model
- ``SpecuSim()``: Explores speculative execution between blocks
- ``IsValueInCache()``: Queries cache state for an instruction
- ``visitLoadInst()``: Updates observations for load instructions
- ``getResult()`` / ``getFindings()``: Access structured findings and counters

**Configuration**:

The analysis supports several configuration options:

- speculation depth
- merge behavior for abstract cache states
- cache parameters such as line size, line count, and set count

Result Model
------------

The public header exposes structured result types:

- ``SpectreObservation`` for individual cache-relevant observations
- ``SpectreFinding`` for branch-local vulnerability evidence
- ``SpectreAnalysisResult`` for aggregated findings and hit/miss counters

Usage
-----

The Spectre analysis is typically used in security auditing pipelines to detect
cache-based speculative side-channel vulnerabilities.

**Typical use cases**:

- Detecting Spectre-variant vulnerabilities
- Analyzing cache timing side-channels
- Security auditing of sensitive low-level code
- Studying speculative divergence in cache behavior

**Dependencies**:

- Dominator Tree
- Post-Dominator Tree
- Alias Analysis

**Limitations**:

- Analysis is intra-procedural
- Requires precise alias analysis for accurate results
- Cache model is simplified compared to real hardware
- Focuses on Spectre-v1-style branch/speculation scenarios rather than all
  speculative-execution variants

Related Components
------------------

- :doc:`crypto` documents the ``ConstantTime`` subsystem, which focuses on
  CT-LLVM-style constant-time checking.
- :doc:`lif` documents the ``LIF`` subsystem for isochronous transformation and
  taint-driven mitigation.
