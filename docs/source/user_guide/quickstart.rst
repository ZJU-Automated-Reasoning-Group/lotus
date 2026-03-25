Quick Start Guide
==================

Get up and running with Lotus quickly.

Basic Usage
-----------

Compile your C/C++ code to LLVM IR:

.. code-block:: bash

   clang -emit-llvm -c example.c -o example.bc
   clang -emit-llvm -S example.c -o example.ll

Alias Analysis
--------------

.. code-block:: bash

   ./build/bin/sparrow-aa example.bc           # SparrowAA (CI mode by default) analysis
   ./build/bin/dyck-aa example.bc            # Unification-based analysis
   ./build/bin/aser-aa example.bc            # AserPTA (CI mode by default) analysis
   ./build/bin/fpa example.bc                # Function pointer analysis
   ./build/bin/lotus-aa example.bc           # Inclusion-based, flow-sensitive context-sensitive
   ./build/bin/sea-dsa-dg --sea-dsa-dot example.bc  # Unification-based, flow-insensitive, context-sensitive
   ./build/bin/seadsa-tool --sea-dsa-dot --outdir results/ example.bc


Bug Detection
-------------

.. code-block:: bash

   # Integer and array bugs
   ./build/bin/lotus-kint -check-int-overflow example.ll  # Integer overflow
   ./build/bin/lotus-kint -check-array-oob example.ll     # Array out of bounds
   ./build/bin/lotus-kint -check-all example.ll           # All checks
   
   # Memory safety bugs
   ./build/bin/lotus-pulse example.bc                        # Memory safety checks

    # IFDS-based, taint-style bugs  
   ./build/bin/lotus-taint example.bc                # Basic taint analysis
   ./build/bin/lotus-taint -sources="read,scanf" -sinks="system,exec" example.bc

   # Concurrency bugs
   ./build/bin/lotus-concur example.bc            # Concurrency bug detection


Abstract Interpretation
------------------------

.. code-block:: bash

   ./build/bin/clam example.bc                    # Clam analyzer
   ./build/bin/clam-pp example.bc                 # Clam pretty-printer
   ./build/bin/clam-diff old.bc new.bc            # Differential analysis

Program Dependence Graph
------------------------

.. code-block:: bash

   ./build/bin/pdg-query example.bc               # Query PDG


Dynamic Validation
------------------

.. code-block:: bash

   ./build/bin/dynaa-instrument example.bc -o example.inst.bc
   clang example.inst.bc libRuntime.a -o example.inst
   LOG_DIR=logs/ ./example.inst
   ./build/bin/dynaa-check example.bc logs/pts.log basic-aa

Example Analysis
----------------

Analyze a vulnerable C program:

.. code-block:: c

   // example.c
   #include <stdio.h>
   
   void vulnerable_function(char* input) {
       char buffer[100];
       strcpy(buffer, input);  // Potential buffer overflow
       printf("%s", buffer);
   }
   
   int main() {
       char user_input[200];
       scanf("%s", user_input);  // Source of tainted data
       vulnerable_function(user_input);
       return 0;
   }

Analysis commands:

.. code-block:: bash

   clang -emit-llvm -c example.c -o example.bc
   clang -emit-llvm -S example.c -o example.ll
   ./build/bin/lotus-taint example.bc                   # Detect taint flow
   ./build/bin/lotus-kint -check-array-oob example.ll   # Check buffer overflow
   ./build/bin/lotus-pulse example.bc                        # Memory safety checks
