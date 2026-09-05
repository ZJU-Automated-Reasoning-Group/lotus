===========================
AserPTA — Pointer Analysis
===========================

Overview
========

AserPTA is a **configurable pointer analysis framework** providing multiple
context sensitivities, memory models, and solver algorithms. It targets
large C/C++ programs and supports both **whole-program** and **modular**
analyses.

* **Location**: ``lib/Alias/InclusionBased/AserPTA``
* **Design**: Constraint-based, graph-driven
* **Key Features**:
  - Multiple context sensitivities (CI, K-call-site, K-origin)
  - Field-insensitive and field-sensitive memory models
  - Several solver backends optimized for scalability

Architecture
============

The analysis pipeline is organized as:

.. code-block:: text

   LLVM IR
      ↓
   [Preprocessing]
      ├─ Canonicalize GEP instructions
      ├─ Lower memcpy operations
      ├─ Normalize heap APIs
      └─ Remove exception handlers
      ↓
   [Constraint Collection]
      ├─ addr_of: p = &obj
      ├─ copy: p = q
      ├─ load: p = *q
      ├─ store: *p = q
      └─ offset: p = &obj->field
      ↓
   [Constraint Graph Construction]
      ├─ Pointer nodes (CGPtrNode)
      ├─ Object nodes (CGObjNode)
      └─ Constraint edges
      ↓
   [Solving with Context]
      ├─ Context evolution (NoCtx/KCallSite/KOrigin)
      ├─ Propagation strategy
      └─ SCC detection & collapsing
      ↓
   Points-to Sets (BitVector representation)

1. **IR Preprocessing** (``PreProcessing/``)
   - Canonicalizes GEPs, lowers ``memcpy``, normalizes heap APIs.
   - Removes exception handlers and inserts synthetic initializers.
2. **Constraint Collection** (``PointerAnalysis/``)
   - Extracts five core constraint types:
     ``addr_of``, ``copy``, ``load``, ``store``, ``offset``.
3. **Constraint Graph Construction**
   - Nodes: pointer nodes (CGPtrNode), object nodes (CGObjNode),
     SCC nodes (CGSuperNode).
4. **Solving with Context**
   - Context models: ``NoCtx``, ``KCallSite<K>``, ``KOrigin<K>``.
   - Solver choices: WavePropagation, DeepPropagation,
     PartialUpdateSolver.

Constraint Types
================

1. **Address-of** (``p = &obj``): Pointer directly addresses object
2. **Copy** (``p = q``): Pointer assignment
3. **Load** (``p = *q``): Dereference and copy
4. **Store** (``*p = q``): Store through pointer
5. **Offset** (``p = &obj->field``): Field access via GEP

Memory Models
=============

- **Field-Insensitive**: Treats objects as single entities (faster)
- **Field-Sensitive**: Models individual fields (more precise)

Analysis Modes and Options
===========================

**Analysis Modes:**
* ``ci``: Context-insensitive (default)
* ``1-cfa``: 1-call-site sensitive
* ``2-cfa``: 2-call-site sensitive  
* ``origin``: Origin-sensitive (tracks thread creation)

**Solver Types:**
* ``basic``: PartialUpdateSolver
* ``wave``: WavePropagation with SCC detection (default)
* ``deep``: DeepPropagation with cycle detection

**Key Options:**
* ``-analysis-mode=<mode>`` – Analysis mode (ci, 1-cfa, 2-cfa, origin)
* ``-solver=<type>`` – Solver algorithm (basic, wave, deep)
* ``-field-sensitive`` – Use field-sensitive memory model (default: true)
* ``-dump-stats`` – Print analysis statistics
* ``-consgraph`` – Dump constraint graph to DOT file
* ``-dump-pts`` – Dump points-to sets

Usage
=====

The standalone driver can be invoked as:

.. code-block:: bash

   ./build/bin/lotus-alias-aser-aa -analysis-mode=1-cfa -solver=deep example.bc

   # Context-insensitive with wave propagation
   ./build/bin/lotus-alias-aser-aa input.bc

   # 1-CFA with deep propagation
   ./build/bin/lotus-alias-aser-aa -analysis-mode=1-cfa -solver=deep input.bc

   # Origin-sensitive (tracks pthread_create and spawns)
   ./build/bin/lotus-alias-aser-aa -analysis-mode=origin input.bc

   # Field-insensitive for faster analysis
   ./build/bin/lotus-alias-aser-aa -field-sensitive=false input.bc

In Lotus, AserPTA is also accessible through the AA wrapper and configuration
files that select it as the primary alias analysis engine.
