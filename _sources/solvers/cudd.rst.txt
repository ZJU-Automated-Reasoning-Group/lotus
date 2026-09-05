CUDD (Binary Decision Diagrams)
===============================

BDD-based symbolic manipulation and decision procedures for Boolean problems.

Overview
--------

The CUDD backend provides a high-performance implementation of Binary Decision
Diagrams (BDDs) that can be used to encode and manipulate Boolean functions and
symbolic sets.

**Location**: ``third-party/CUDD/``

**Main capabilities**:

- Canonical representation of Boolean functions.
- Efficient Boolean operations (AND, OR, NOT, implication, equivalence, etc.).
- Symbolic sets and relations represented as BDDs.
- Support for fixed-point style computations over BDDs.

Typical Use Cases
-----------------

- Symbolic state-space exploration and model checking.
- Boolean abstraction layers in larger analyses.
- Compact representation of large sets or relations.

Basic Usage (C\+\+)
-------------------

.. code-block:: cpp

   #include <CUDD/cudd.h>

   DdManager *Manager = Cudd_Init(0, 0, CUDD_UNIQUE_SLOTS, 127, 0);
   DdNode *X = Cudd_bddIthVar(Manager, 0);
   Cudd_Quit(Manager);

Features
--------

- **BDD operations** – Canonical representation and manipulation of Boolean
  functions.
- **Symbolic sets** – Encoding of sets as BDDs with efficient union,
  intersection, and projection.
- **Fixed-point computation** – Iterative algorithms over BDDs for reachability
  and invariance problems.

Integration Notes
-----------------

The CUDD backend is typically not used directly by end users. Instead, it is
used by higher-level applications and analyses that require symbolic Boolean
reasoning. See :doc:`index` for an overview of where CUDD fits in the solver
stack.
