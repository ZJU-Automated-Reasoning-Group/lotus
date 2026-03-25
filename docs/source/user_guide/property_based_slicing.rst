Property-Based Slicing
======================

Property-based slicing allows you to slice programs with respect to specific verification properties, reducing the program to only the parts relevant to checking that property.

Overview
--------

Property-based slicing extends Lotus's PDG slicing capabilities by allowing you to specify properties using Symbiotic-compatible `.prp` files. The slicer automatically identifies relevant program points based on the property and computes backward or forward slices.

Property Specification Format
------------------------------

Lotus supports Symbiotic-style property files with LTL/FQL syntax:

**Safety Properties (CHECK):**

.. code-block:: text

   CHECK( init(main()), LTL(G ! call(reach_error())) )
   CHECK( init(main()), LTL(G valid-deref) )
   CHECK( init(main()), LTL(G valid-free) )
   CHECK( init(main()), LTL(G valid-memtrack) )
   CHECK( init(main()), LTL(G ! overflow) )
   CHECK( init(main()), LTL(G ! null-deref) )
   CHECK( init(main()), LTL(G def-behavior) )
   CHECK( init(main()), LTL(G valid-memcleanup) )
   CHECK( init(main()), LTL(F end) )

**Coverage Properties (COVER):**

.. code-block:: text

   COVER( init(main()), FQL(COVER EDGES(@DECISIONEDGE)) )
   COVER( init(main()), FQL(COVER EDGES(@BASICBLOCKENTRY)) )
   COVER( init(main()), FQL(COVER EDGES(@CONDITIONEDGE)) )
   COVER( init(main()), FQL(COVER EDGES(@CALL(reach_error))) )

Supported Property Types
------------------------

* **UnreachCall**: Target function should not be reachable
* **MemSafety**: Memory safety (valid-deref, valid-free, valid-memtrack)
* **NoOverflow**: No signed integer overflow
* **Termination**: Program terminates
* **NullDeref**: No null pointer dereference
* **DefBehavior**: No undefined behavior
* **MemCleanup**: Memory cleanup safety
* **CoverageBranches**: Branch coverage
* **CoverageStatements**: Statement coverage
* **CoverageConditions**: Condition coverage
* **CoverageErrorCall**: Error call coverage

Usage
-----

Basic usage with ``pdg-query``:

.. code-block:: bash

   # Backward slice (default)
   pdg-query input.bc --property-file property.prp

   # Forward slice
   pdg-query input.bc --property-file property.prp --direction forward

   # Dump slice nodes
   pdg-query input.bc --property-file property.prp --dump-slice

Example
-------

Given a property file ``unreach-call.prp``:

.. code-block:: text

   CHECK( init(main()), LTL(G ! call(reach_error())) )

And a program ``test.c``:

.. code-block:: c

   extern void reach_error();
   
   int main() {
     int x = 0;
     if (x > 0) {
       reach_error();
     }
     return 0;
   }

Slice the program:

.. code-block:: bash

   clang -emit-llvm -c test.c -o test.bc
   pdg-query test.bc --property-file unreach-call.prp

The slicer will identify the ``reach_error()`` call and compute a backward slice containing all instructions that can affect whether that call is executed.

Integration with Verification
------------------------------

Property-based slicing is designed to work with Lotus's verification backends:

.. code-block:: bash

   pdg-query input.bc --property-file property.prp --dump-slice

This workflow identifies the PDG region relevant to the property so it can be
fed into verification or reduction passes without requiring a separate
frontend binary.
