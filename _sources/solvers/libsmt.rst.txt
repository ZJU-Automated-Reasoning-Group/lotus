LIBSMT — SMT Solver Abstraction Layer
=====================================

A unified abstraction layer over multiple SMT solver backends.

**Headers**: ``include/Solvers/SMT/LIBSMT/``

**Implementation**: ``lib/Solvers/SMT/LIBSMT/``

**Build target**: ``CanarySMT``

Overview
--------

LIBSMT provides a solver-agnostic interface for SMT (Satisfiability Modulo
Theories) solving. It abstracts over solver-specific details (Z3, etc.)
so that Lotus analyses can issue SMT queries without depending on any single
solver backend.

Components
----------

SMTSolver
~~~~~~~~~

Abstract base class defining the solver interface:

- ``push()`` / ``pop()`` — Assertion stack management.
- ``assertExpr(SMTExpr)`` — Add a formula to the solver context.
- ``check()`` — Check satisfiability of the current context.
- ``getModel()`` — Retrieve a satisfying model.
- ``reset()`` — Clear all assertions.

SMTExpr and SMTObject
~~~~~~~~~~~~~~~~~~~~~

Expression tree representation used to build formulas:

- ``SMTExpr`` — Typed expression with a solver-specific handle.
- ``SMTObject`` — Base class for all SMT entities (sorts, functions, variables).

SMTFactory
~~~~~~~~~~

Factory for creating SMT sorts, variables, and expressions:

.. code-block:: cpp

   #include "Solvers/SMT/LIBSMT/SMTFactory.h"

   auto &factory = SMTFactory::instance();
   auto intSort  = factory.getSort("Int");
   auto x        = factory.makeVariable(intSort, "x");
   auto y        = factory.makeVariable(intSort, "y");
   auto expr     = factory.makeEQ(x, y);

Z3Expr and Z3Plus
~~~~~~~~~~~~~~~~~

Z3-specific expression handling and extended operations (quantifier support,
optimisation queries, model parsing).

CNF and SATSolver
~~~~~~~~~~~~~~~~~

CNF formula representation and SAT solver interface for boolean-level
reasoning:

- ``CNF`` — Conjunctive normal form representation.
- ``SATSolver`` — SAT solver wrapper (useful for eager bit-blasting).

SMTModel
~~~~~~~~

Represents a satisfying assignment from a solver query, providing value lookup
by variable name.

SMTConfigure
~~~~~~~~~~~~

Configuration and option management for solver backends.

Usage
-----

.. code-block:: cpp

   #include "Solvers/SMT/LIBSMT/SMTSolver.h"
   #include "Solvers/SMT/LIBSMT/SMTFactory.h"

   auto &factory = SMTFactory::instance();
   auto solver   = factory.createSolver();

   auto intSort = factory.getSort("Int");
   auto x       = factory.makeVariable(intSort, "x");
   auto gt      = factory.makeGT(x, factory.makeIntVal(0));

   solver->assertExpr(gt);
   auto result = solver->check();

   if (result == SmtResult::Sat) {
     auto model = solver->getModel();
     auto val   = model->getValue(x);
     // ...
   }

See Also
--------

- :doc:`./smt` — SMT solver backend (Z3)
- :doc:`./cudd` — BDD solver backend (CUDD)
- :doc:`./wpds` — Weighted pushdown systems
