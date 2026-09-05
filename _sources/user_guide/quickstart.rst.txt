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

   ./build/bin/lotus-alias-sparrow-aa example.bc    # SparrowAA (CI mode by default) analysis
   ./build/bin/lotus-alias-dyck-aa example.bc       # Unification-based analysis
   ./build/bin/lotus-alias-aser-aa example.bc       # AserPTA (CI mode by default) analysis
   ./build/bin/lotus-alias-fpa example.bc           # Function pointer analysis
   ./build/bin/lotus-alias-lotus-aa example.bc      # Inclusion-based, flow-sensitive context-sensitive
   ./build/bin/lotus-alias-sea-dsa-dg --sea-dsa-dot example.bc  # Unification-based, flow-insensitive, context-sensitive
   ./build/bin/lotus-alias-seadsa-tool --sea-dsa-dot --outdir results/ example.bc


Bug Detection
-------------

.. code-block:: bash

   # Integer and array bugs
   ./build/bin/lotus-check --engine=kint example.ll --checks=int-overflow  # Integer overflow
   ./build/bin/lotus-check --engine=kint example.ll --checks=array-oob     # Array out of bounds
   ./build/bin/lotus-check --engine=kint example.ll --checks=all           # All KINT checks
   
   # Memory safety: choose the engine for the workflow.
   ./build/bin/lotus-check --engine=fitx example.bc                  # Fast feedback
   ./build/bin/lotus-check --engine=ae example.bc --checks=all       # Broad AE pass
   ./build/bin/lotus-check --engine=pulse example.bc                 # Witness-oriented checks
   ./build/bin/lotus-check --engine=saber example.bc --checks=all    # Leaks and double frees

    # IFDS-based, taint-style bugs  
   ./build/bin/lotus-check --engine=taint example.bc                    # Basic taint analysis
   ./build/bin/lotus-check --engine=taint example.bc --taint.sources=read,scanf --taint.sinks=system,exec

   # Concurrency bugs
   ./build/bin/lotus-check --engine=concur example.bc            # Concurrency bug detection

   # See docs/source/checker/index.rst for the complete bug-class guide.


Abstract Interpretation
------------------------

.. code-block:: bash

   ./build/bin/clam example.bc                    # Clam analyzer
   ./build/bin/clam-pp example.bc                 # Clam pretty-printer
   ./build/bin/clam-diff old.bc new.bc            # Differential analysis

Program Dependence Graph
------------------------

.. code-block:: bash

   ./build/bin/lotus-ir-pdg-query example.bc      # Query PDG

The PDG query frontend is implemented in
``tools/ir/lotus-ir-pdg-query.cpp``.


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
   ./build/bin/lotus-check --engine=taint example.bc                 # Detect taint flow
   ./build/bin/lotus-check --engine=kint example.ll --checks=array-oob # Check array bounds
   ./build/bin/lotus-check --engine=pulse example.bc                 # Memory safety checks
