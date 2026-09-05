E-Graph Quantifier Simplification (EGraphsSimp)
================================================

This section documents the E-Graph based quantifier simplification utilities
for SMT formulas.

Overview
--------

**Location**: ``lib/Solvers/SMT/QuantSimp/``

**Main Header**: ``Solvers/SMT/QuantSimp/EGraphsSimp.h``

This module provides E-graph based data structures and algorithms for handling
quantified SMT formulas. It implements term indexing and congruence closure
algorithms useful for quantifier instantiation and formula simplification.

Key Components
---------------

EGraph Class
~~~~~~~~~~~~

The core ``EGraph`` class maintains a term index for efficient pattern matching
and equality reasoning in quantified formulas.

**Features**:
- Term indexing for quantified formulas
- Congruence closure for equality reasoning
- Support for quantifier patterns and weights
- Efficient term lookup and matching

**Header**:
.. code-block:: cpp

   #include "Solvers/SMT/QuantSimp/EGraphsSimp.h"

**Usage**:
.. code-block:: cpp

   #include "Solvers/SMT/QuantSimp/EGraphsSimp.h"

   using namespace EGraphs;

   z3::context ctx;
   z3::expr formula = /* ... */;

   // Simplify using E-Graph
   auto simplified = EGraph::Simplify(formula, &ctx);

Function Class
~~~~~~~~~~~~~~

Represents functions/terms within the E-graph structure.

**Features**:
- Supports quantified and unquantified terms
- Tracks usage relationships between terms
- Implements equality and congruence checking

**Methods**:
- ``getName()`` - Get the function declaration
- ``GetRoot()`` - Find the representative in the congruence closure
- ``IsEquivalent()`` - Test for exact equality
- ``IsCongruent()`` - Check congruence (same function with congruent arguments)

QuantifierArgs Class
~~~~~~~~~~~~~~~~~~~~

Stores quantifier metadata including:

- ``is_forall`` - Universal or existential quantification
- ``weight`` - Quantifier weight for heuristics
- ``num_patterns`` - Number of patterns attached
- ``num_decls`` - Number of bound variables
- ``sorts`` - Sorts of bound variables
- ``decl_names`` - Names of bound variables
- ``patterns`` - Attached patterns

Main Functions
--------------

ExprToEGraph
~~~~~~~~~~~~~

Parses a Z3 expression and builds an E-graph representation.

.. code-block:: cpp

   z3::expr expr = /* ... */;
   EGraph *graph = EGraph::ExprToEGraph(expr, &ctx);

ToFormula
~~~~~~~~~

Converts an E-graph back to a Z3 formula.

.. code-block:: cpp

   std::map<Function *, Function *> repr;
   std::set<Function *> core;
   z3::expr formula = graph->ToFormula(&repr, &core);

Simplify (Static)
~~~~~~~~~~~~~~~~~~

High-level simplification interface.

.. code-block:: cpp

   z3::expr simplified = EGraph::Simplify(formula, &ctx);

Algorithms
----------

The module implements several algorithms:

1. **Congruence Closure** - Efficient equality reasoning
2. **E-matching** - Pattern matching in quantified formulas
3. **Quantifier Instantiation** - Heuristic quantifier handling
4. **Term Indexing** - Fast pattern lookup

Use Cases
---------

- Simplifying quantified SMT formulas
- Pattern matching in verification conditions
- Equality reasoning with uninterpreted functions
- Building quantifier instantiation heuristics

Related Components
------------------

- See :doc:`smt` - Core SMT solver infrastructure
- See :doc:`smtsampler` - SMT sampling (may use simplification)
