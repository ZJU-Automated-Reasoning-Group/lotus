Verification Frontend
====================

BooleanProgram
--------------

**Location**: ``lib/Verification/Frontend/``

**Status**: Internal/Experimental — not exposed via any production tool.

A parser+lowerer for a Boolean/predicate program specification language
(Bebop/SATABS-style). Reads a textual format where programs consist of
predicates with procedures, control-flow statements, and BooleanExpr terms.
Lowers parsed programs to the NPA dataflow framework via
``PredicateProgramLowering``.

**Components**:
- ``BooleanProgramParser`` — recursive-descent parser (hand-written)
- ``BooleanProgram`` — AST data structures (Procedure, Statement, BooleanExpr, etc.)
- ``PredicateProgramLowering`` — lowers to NPA ``PredicateRelation``-based CFG

Only linked by unit tests (``boolean_program_frontend_test``).

Scope and usage
---------------

This frontend is intended for experiments that begin with predicate-program
input rather than LLVM IR.  Its lowering produces the relations and control
flow consumed by NPA; it is not a general parser for C, C++, or arbitrary
Boogie.  Treat the input language and lowering behavior as internal while the
component remains experimental, and use the unit test as the most reliable
executable example.
