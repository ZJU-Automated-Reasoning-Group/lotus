SMT Theory Arbitrage (STAUB)
===========================

This section documents the ``STAUB`` (SMT Theory Arbitrage, from Unbounded to Bounded)
library for accelerating SMT solving of unbounded constraints.

Overview
--------

**Location**: ``lib/Solvers/SMT/STAUB/``

**Origin**: https://github.com/mikekben/STAUB

STAUB speeds up SMT solving of unbounded constraints by converting them to bounded
theories. Its bounded formulas can be translated to LLVM IR by TUNA's
``SMT2LLVM`` tools.

Key Concept
-----------

Unbounded integer/real arithmetic is often slow for SMT solvers. STAUB:

1. **Analyzes** unbounded constraints in the original formula
2. **Computes bounds** using optimization queries
3. **Transforms** the formula to a bounded bit-vector representation
4. **Translates** to LLVM IR via TUNA's SMT2LLVM tools for efficient solving

This approach can provide significant speedups for certain classes of problems.

Components
----------

SMTFormula (STAUB variant)
~~~~~~~~~~~~~~~~~~~~~~~~~~

The STAUB version of SMTFormula adds bound computation and transformation.

**Header**:
.. code-block:: cpp

   #include "Solvers/SMT/STAUB/SMTFormula.h"

**Constructor**:
.. code-block:: cpp

   STAUB::SMTFormula(
       z3::context &scx,        // Z3 context for SMT solving
       LLVMContext &lcx,        // LLVM context
       Module *lmodule,         // Output module
       IRBuilder<> &builder,    // IR builder
       std::string smt_string   // SMT-LIB formula
   );

**Methods**:
- ``ToLLVM()`` - Generate LLVM IR function
- ``LargestIntegerConstant()`` - Find maximum integer constant in formula
- ``AbstractSingle()`` - Compute single-variable bounds
- ``AbstractFloat()`` - Compute floating-point bounds
- ``ToSMT()`` - Convert to bounded SMT with specified width
- ``ToSMTFloat()`` - Convert to floating-point SMT

SMTNode (STAUB variant)
~~~~~~~~~~~~~~~~~~~~~~~~

Extended node class supporting STAUB's bound-aware operations.

**Header**:
.. code-block:: cpp

   #include "Solvers/SMT/STAUB/SMTNode.h"

STAUBUtil
~~~~~~~~~

Utility functions for bound computation:

**Header**:
.. code-block:: cpp

   #include "Solvers/SMT/STAUB/STAUBUtil.h"

**Functions**:
- ``APMax()`` - Maximum of two APInt values
- ``APPlus()`` - Addition with overflow-aware widening
- ``APMult()`` - Multiplication with overflow-aware widening
- ``APDiv()`` - Division (with under-approximation)
- ``PairMax()`` - Max for (value, precision) pairs
- ``PairPlus()`` - Plus for (value, precision) pairs
- ``PairMult()`` - Multiply for (value, precision) pairs
- ``PairDiv()`` - Divide for (value, precision) pairs

STAUBExceptions
~~~~~~~~~~~~~~~

Custom exceptions:

.. code-block:: cpp

   namespace STAUB {
       class UnsupportedSMTTypeException;
       class UnsupportedSMTOpException;
       class UnsupportedLLVMOpException;
   }

Usage Example
-------------

.. code-block:: cpp

   #include "Solvers/SMT/STAUB/SMTFormula.h"
   #include "Solvers/SMT/STAUB/STAUBUtil.h"

   using namespace STAUB;

   std::string smt = R"(
       (declare-fun x () Int)
       (declare-fun y () Int)
       (assert (> x 0))
       (assert (< x 100))
       (assert (= y (+ x 10)))
   )";

   z3::context scx;
   LLVMContext lcx;
   Module *M = new Module("staub_test", lcx);
   IRBuilder<> builder(lcx);

   SMTFormula formula(scx, lcx, M, builder, smt);

   // Compute bounds
   auto max_x = formula.LargestIntegerConstant();
   auto bounds = formula.AbstractSingle(max_x);

   // Generate LLVM IR for bounded solving
   formula.ToLLVM();

Workflow
--------

1. **Parse**: Read unbounded SMT-LIB formula
2. **Analyze**: Compute variable bounds using Z3 optimize
3. **Bound**: Transform to bounded representation
4. **Translate**: Use TUNA's SMT2LLVM tools to generate LLVM IR
5. **Solve**: Use LLVM-based or bounded SMT solving

Related Work
------------

STAUB is based on the paper:
"SMT Theory Arbitrage: From Unbounded to Bounded Solving"

See Also
--------

- See ``lib/Solvers/SMT/TUNA/TUNA-Opt/SMT2LLVM`` for SMT-LIB to LLVM
  translation
- See :doc:`symabs` - Symbolic abstraction (related techniques)
