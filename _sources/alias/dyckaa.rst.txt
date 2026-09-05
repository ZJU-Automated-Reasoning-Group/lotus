==========================
DyckAA — Dyck-CFL Analysis
==========================

Overview
========

DyckAA is a **high-precision, unification-based alias analysis** that uses
Dyck **Context-Free Language (CFL) reachability** to model pointer
relationships. It is designed for **maximum precision** and is well suited to
function-pointer resolution and detailed value-flow reasoning.

* **Location**: ``lib/Alias/UnificationBased/DyckAA``
* **Context**: Context-insensitive
* **Precision**: Field-sensitive, CFL-based alias sets

Core Idea
=========

DyckAA builds a **Dyck graph** whose edges are labeled with operations that
behave like balanced parentheses:

* ``(*`` / ``*)`` — dereference and reference
* ``[field`` / ``field]`` — field access and projection
* Assignment and copy edges

Alias relationships correspond to balanced paths in this labeled graph. The
Dyck-CFL reachability algorithm discovers such paths and **unifies**
equivalent nodes into alias sets.

Algorithm
=========

The analysis uses Dyck Context-Free Language (CFL) reachability to model pointer relationships:

.. code-block:: text

   LLVM IR
      ↓
   [Build Dyck Graph]
      ├─ Nodes: Values (pointers, objects)
      ├─ Edges with labels:
      │  ├─ *(* : Dereference
      │  ├─ *)* : Reference
      │  ├─ *[field]* : Field access
      │  └─ Assignment edges
      ↓
   [Dyck-CFL Reachability]
      ├─ Find balanced paths
      ├─ Unify equivalent nodes
      └─ Build alias sets
      ↓
   [Applications]
      ├─ Alias queries
      ├─ Call graph construction
      ├─ ModRef analysis
      └─ Value flow analysis

Dyck Language
=============

Balanced parentheses language that captures pointer semantics:

- ``( ... )`` : Dereference operations must balance
- ``[ ... ]`` : Field accesses must match
- Paths between nodes indicate aliasing

Example
-------

.. code-block:: c

   int x;
   int *p = &x;    // Edge: p -*)->* x
   int **q = &p;   // Edge: q -*)->* p
   int *r = *q;    // Path: r = *q, q points to p, so r = p
                   // Dyck path: r -*(*-* q -*)->* p

Capabilities
============

DyckAA provides:

* Precise **alias queries** (may/must sets).
* Construction of **call graphs** for indirect calls.
* **ModRef** information (modified/referenced memory).
* **Value-flow graphs** (DyckVFG) for downstream analyses.

Strengths
=========

- Highly precise through CFL reachability
- Handles complex pointer patterns
- Good for function pointer resolution
- Builds precise call graphs

Limitations
===========

- Computationally expensive
- High memory usage for large programs
- Context-insensitive (single analysis per function)

Usage
=====

DyckAA is typically run via its dedicated tool:

.. code-block:: bash

   ./build/bin/lotus-alias-dyck-aa -print-alias-set-info example.bc

Available Options
-----------------

* ``-print-alias-set-info``
  
  Prints the evaluation of alias sets and outputs all alias sets and their
  relations (DOT format).

* ``-count-fp``
  
  Counts how many functions a function pointer may point to.

* ``-no-function-type-check``
  
  If set, disables function type checking when resolving pointer calls.
  Otherwise, only FuncTy-compatible functions can be aliased with a function
  pointer. Two functions f1 and f2 are FuncTy-compatible if:
  
  - Both or neither are variadic functions
  - Both or neither have a non-void return value
  - They have the same number of parameters
  - Parameters have the same FuncTy store sizes
  - There is an explicit cast operation between FuncTy(f1) and FuncTy(f2)
    (works with ``-with-function-cast-comb`` option)

* ``-dot-dyck-callgraph``
  
  Prints a call graph based on the alias analysis. Can be used with
  ``-with-labels`` option to add labels (call instructions) to the edges in
  call graphs.

Additional flags enable call graph export, function pointer statistics, and
DOT visualizations of internal graphs.

Advanced Features
=================

- **DyckVFG**: Value Flow Graph construction for tracking value propagation
- **ModRef Analysis**: Modified/Referenced analysis for optimization
- **Call Graph**: Precise indirect call resolution
