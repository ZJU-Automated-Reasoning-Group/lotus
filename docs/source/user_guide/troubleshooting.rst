Troubleshooting and FAQ
========================

This document helps resolve common issues and answers frequently asked questions.

Installation Issues
-------------------

LLVM Version Mismatch
~~~~~~~~~~~~~~~~~~~~~

**Problem**: CMake can't find LLVM or finds wrong version.

.. code-block:: text

   CMake Error: Could not find LLVM
   -- or --
   Found LLVM 15.0.0 but need 14.0.0

**Solution**:

1. Check that LLVM 14.x is installed and visible on ``PATH``:

.. code-block:: bash

   llvm-config --version

2. If multiple LLVM versions are installed, point CMake to the desired one explicitly:

.. code-block:: bash

   cmake ../ -DLLVM_BUILD_PATH=/path/to/llvm-14/lib/cmake/llvm

Z3 Not Found
~~~~~~~~~~~~

**Problem**: Z3 library or headers not found during build.

.. code-block:: text

   Could not find Z3 libraries
   fatal error: z3.h: No such file or directory

**Solution**:

1. Install Z3 development package:

.. code-block:: bash

   # Ubuntu/Debian
   sudo apt-get install libz3-dev
   
   # macOS
   brew install z3
   
   # Or build from source
   git clone https://github.com/Z3Prover/z3
   cd z3
   python scripts/mk_make.py
   cd build && make && sudo make install

2. Specify Z3 path:

.. code-block:: bash

   cmake ../ -DZ3_ROOT=/path/to/z3/install

Boost Not Found
~~~~~~~~~~~~~~~

**Problem**: Boost libraries required for SeaHorn, CLAM, and/or CclyzerAA not found.

.. code-block:: text

   Could NOT find Boost

**Solution**:

1. Install Boost:

.. code-block:: bash

   # Ubuntu/Debian
   sudo apt-get install libboost-all-dev
   
   # macOS
   brew install boost

2. Or specify custom Boost:

.. code-block:: bash

   cmake ../ -DCUSTOM_BOOST_ROOT=/path/to/boost

3. Lotus can auto-download Boost:

.. code-block:: bash

   cmake ../ -DAUTO_DOWNLOAD_BOOST=ON

Compilation Errors
~~~~~~~~~~~~~~~~~~

**Problem**: Undefined reference errors during linking.

.. code-block:: text

   undefined reference to `llvm::PassRegistry::registerPass'

**Solution**:

1. Ensure LLVM was built with:

.. code-block:: bash

   cmake -DLLVM_ENABLE_RTTI=ON -DLLVM_ENABLE_EH=ON ../llvm

2. Clean and rebuild:

.. code-block:: bash

   cd build
   rm -rf *
   cmake ../
   make -j$(nproc)

Analysis Issues
---------------

Analysis Crashes
~~~~~~~~~~~~~~~~

**Problem**: Tool crashes with segmentation fault.

.. code-block:: text

   Segmentation fault (core dumped)

**Solution**:

1. Run with debug build:

.. code-block:: bash

   cmake ../ -DCMAKE_BUILD_TYPE=Debug
   make
   gdb --args ./bin/tool input.bc
   (gdb) run
   (gdb) backtrace

2. Check input validity:

.. code-block:: bash

   opt -verify input.bc -o /dev/null

3. Common causes:
   
   - Malformed LLVM IR
   - Null pointer dereference in analysis
   - Stack overflow (increase limit: ``ulimit -s unlimited``)

Analysis Takes Too Long
~~~~~~~~~~~~~~~~~~~~~~~

**Problem**: Analysis runs indefinitely or for hours.

**Solution**:

1. Use faster analysis mode:

.. code-block:: bash

   # For alias analysis, use context-insensitive
   ./bin/lotus-alias-aser-aa -analysis-mode=ci input.bc
   
   # For CLAM, use interval domain
   ./bin/clam --crab-dom=int input.bc

2. Set timeout:

.. code-block:: bash

   timeout 300 ./bin/tool input.bc  # 5 minutes

3. For Kint, set function timeout:

.. code-block:: bash

   ./build/bin/lotus-check --engine=kint input.ll --kint.function-timeout-seconds=30

4. Reduce precision:

.. code-block:: bash

   # Disable field sensitivity
   ./bin/lotus-alias-aser-aa -field-sensitive=false input.bc
   
   # Limit context depth
   ./bin/lotus-alias-aser-aa -analysis-mode=1-cfa -max-context-depth=2 input.bc

Out of Memory
~~~~~~~~~~~~~

**Problem**: Analysis exhausts system memory.

.. code-block:: text

   std::bad_alloc
   Killed

**Solution**:

1. Increase available memory (if possible)

2. Use more scalable analysis:

.. code-block:: bash

   ./bin/lotus-alias-dyck-aa input.bc

3. Analyze incrementally:

.. code-block:: bash

   # Split module into parts
   llvm-extract -func=main input.bc -o main.bc
   ./bin/tool main.bc

4. Limit analysis scope:

.. code-block:: bash

   # Intraprocedural only
   ./bin/clam --crab-inter=false input.bc

False Positives
~~~~~~~~~~~~~~~

**Problem**: Analysis reports bugs that don't exist.

**Solution**:

1. Use more precise analysis:

.. code-block:: bash

   # 1-CFA instead of CI
   ./bin/lotus-alias-aser-aa -analysis-mode=1-cfa input.bc
   
   # Zones domain instead of intervals
   ./bin/clam --crab-dom=zones input.bc

2. Validate with dynamic analysis:

.. code-block:: bash

   ./bin/dynaa-instrument input.bc -o inst.bc
   clang inst.bc libRuntime.a -o inst
   LOG_DIR=logs ./inst
   ./bin/dynaa-check input.bc logs/pts.log basic-aa

3. Add annotations to code:

.. code-block:: c

   void my_function(int *p) {
       __builtin_assume(p != NULL);  // Help analysis
       *p = 10;
   }

Missing Bugs (False Negatives)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Problem**: Analysis doesn't detect known bugs.

**Solution**:

1. Enable all checkers:

.. code-block:: bash

   ./build/bin/lotus-check --engine=kint input.ll --checks=all

2. Use more comprehensive analysis:

.. code-block:: bash

   # Interprocedural
   ./bin/clam --crab-inter --crab-check=all input.bc
   
   # Context-sensitive taint
   ./build/bin/lotus-check --engine=taint input.bc --verbose

3. Check if bug type is supported:

.. code-block:: bash

   ./build/bin/lotus-check --engine=kint --help  # See supported checks

Tool-Specific Issues
--------------------

CLAM Issues
~~~~~~~~~~~

**Problem**: CLAM fails with "unsupported instruction".

.. code-block:: text

   Unsupported LLVM instruction: <instruction>

**Solution**:

1. Preprocess bitcode:

.. code-block:: bash

   ./bin/clam-pp --crab-lower-unsigned-icmp \
                  --crab-lower-select \
                  input.bc -o prep.bc
   ./bin/clam prep.bc

2. Use suitable domain:

.. code-block:: bash

   # Some domains don't support all operations
   ./bin/clam --crab-dom=int input.bc  # Basic support

**Problem**: CLAM reports "Could not allocate memory".

**Solution**:

1. Use less expensive domain:

.. code-block:: bash

   # Polyhedra is expensive
   ./bin/clam --crab-dom=zones input.bc  # Use zones instead

2. Disable tracking:

.. code-block:: bash

   ./bin/clam --crab-track=num input.bc  # Track only numbers

DyckAA Issues
~~~~~~~~~~~~~

**Problem**: DyckAA produces too many alias pairs.

**Solution**:

1. Enable function type checking:

.. code-block:: bash

   # Don't use -no-function-type-check

2. The analysis is very precise but conservative. This is expected behavior.

**Problem**: Call graph is incomplete.

**Solution**:

.. code-block:: bash

   # Ensure indirect calls are analyzed
   ./bin/lotus-alias-dyck-aa -dot-dyck-callgraph input.bc

PDG Query Issues
~~~~~~~~~~~~~~~~

**Problem**: PDG query returns empty set unexpectedly.

**Solution**:

1. Check node names:

.. code-block:: cypher

   # Function names are case-sensitive
   MATCH (n:FUNC_ENTRY) WHERE n.name = 'Main' RETURN n  # Wrong
   MATCH (n:FUNC_ENTRY) WHERE n.name = 'main' RETURN n  # Correct

2. Verify PDG was built:

.. code-block:: bash

   ./bin/lotus-ir-pdg-query -q "MATCH (n) RETURN n" input.bc  # Should show nodes

3. Use verbose mode:

.. code-block:: bash

   ./bin/lotus-ir-pdg-query -v -q "MATCH (n) RETURN n" input.bc

**Problem**: Query parser error.

**Solution**:

1. Check query syntax:

.. code-block:: cypher

   # Wrong
   MATCH (n:FUNC_ENTRY) WHERE n.name = main RETURN n
   
   # Correct
   MATCH (n:FUNC_ENTRY) WHERE n.name = 'main' RETURN n

2. Use interactive mode to test:

.. code-block:: bash

   ./bin/lotus-ir-pdg-query -i input.bc
   > MATCH (n:FUNC_ENTRY) WHERE n.name = 'main' RETURN n

Taint Analysis Issues
~~~~~~~~~~~~~~~~~~~~~

**Problem**: No taint flows detected but vulnerability exists.

**Solution**:

1. Specify custom sources/sinks:

.. code-block:: bash

   ./build/bin/lotus-check --engine=taint input.bc --taint.sources=my_input_func \
                      --taint.sinks=my_output_func

2. Check that source and sink functions are actually called

3. Use ``lotus-check --engine=taint`` for interprocedural flow:

.. code-block:: bash

   ./build/bin/lotus-check --engine=taint input.bc

Performance Tuning
------------------

Speeding Up Analysis
~~~~~~~~~~~~~~~~~~~~

1. **Choose appropriate analysis**


2. **Reduce scope**:

.. code-block:: bash

   # Analyze specific function
   llvm-extract -func=main input.bc -o main.bc
   ./bin/tool main.bc

3. **Use preprocessing**:

.. code-block:: bash

   # Simplify IR first
   opt -mem2reg -simplifycfg input.bc -o simplified.bc
   ./bin/tool simplified.bc

4. **Parallel analysis** (if tool supports):

.. code-block:: bash


Reducing Memory Usage
~~~~~~~~~~~~~~~~~~~~~

1. **Use sparse representations**:

.. code-block:: bash

   # Field-insensitive uses less memory
   ./bin/lotus-alias-aser-aa -field-sensitive=false input.bc

2. **Limit analysis depth**:

.. code-block:: bash

   ./bin/lotus-alias-aser-aa -analysis-mode=1-cfa input.bc  # Not 2-cfa

3. **Process in batches**:

.. code-block:: bash

   # Analyze files separately
   for f in *.bc; do
       ./bin/tool "$f"
   done

Frequently Asked Questions
--------------------------

General Questions
~~~~~~~~~~~~~~~~~

**Q: Which alias analysis should I use?**

A: Depends on your needs

**Q: Can Lotus analyze C++ code?**

A: Yes! Compile to LLVM IR:

.. code-block:: bash

   clang++ -emit-llvm -c code.cpp -o code.bc
   ./bin/tool code.bc

**Q: Does Lotus support multi-threaded programs?**

A: Yes, use:

- ``lotus-check --engine=concur`` for concurrency bugs
- ``lotus-alias-aser-aa -analysis-mode=origin`` for thread-aware analysis
- MHP analysis in concurrency module

**Q: How do I visualize results?**

A: Generate DOT files:

.. code-block:: bash

   # Call graph
   ./bin/lotus-alias-dyck-aa -dot-dyck-callgraph input.bc
   
   # PDG
   ./bin/lotus-ir-pdg-query -dump-dot input.bc
   
   # Sea-DSA memory graph
   ./bin/lotus-alias-seadsa-tool --sea-dsa-dot --outdir=output/ input.bc
   
   # Convert to PDF
   dot -Tpdf output.dot -o output.pdf

**Q: Can I integrate Lotus into my own tool?**

A: Yes! See :doc:`../developer/api_reference` for programmatic usage.

Analysis Questions
~~~~~~~~~~~~~~~~~~

**Q: What's the difference between flow-sensitive and flow-insensitive?**

A: 

- **Flow-insensitive**: Single points-to set per variable for entire program. Faster but less precise.
- **Flow-sensitive**: Different points-to sets at different program points. Slower but more precise.

Example:

.. code-block:: c

   int x, y;
   int *p;
   p = &x;  // p points to x
   p = &y;  // p points to y
   
   // Flow-insensitive: p may point to {x, y} everywhere
   // Flow-sensitive: p points to {x} at line 4, {y} at line 5

**Q: What's context sensitivity?**

A: Distinguishing different calling contexts:

.. code-block:: c

   void foo(int *p) { *p = 10; }
   
   int main() {
       int x, y;
       foo(&x);  // Context 1
       foo(&y);  // Context 2
   }

- **Context-insensitive**: Analyzes ``foo`` once, ``p`` may point to {x, y}
- **Context-sensitive**: Analyzes ``foo`` twice, ``p`` points to {x} in context 1, {y} in context 2

**Q: How accurate is the analysis?**

A: Lotus analyses are **sound** (no false negatives for most analyses) but may have **false positives**. Trade-off between precision and performance.

**Q: Can I customize source/sink definitions?**

A: Yes:

.. code-block:: bash

   ./build/bin/lotus-check --engine=taint input.bc --taint.sources=my_source1,my_source2 \
                      --taint.sinks=my_sink1,my_sink2

Or create custom analysis (see :doc:`../developer/developer_guide`).

CLAM Questions
~~~~~~~~~~~~~~

**Q: Which abstract domain should I use?**

A:

- **Intervals** (``int``): Fast, simple ranges [l, u]
- **Zones** (``zones``): Moderate, difference constraints (x - y ≤ c)
- **Octagons** (``oct``): More precise, diagonal constraints (±x ± y ≤ c)
- **Polyhedra** (``pk``): Most precise, but expensive

Start with ``zones`` as good balance.

**Q: What's interprocedural analysis?**

A: Analyzes across function boundaries:

.. code-block:: bash

   # Intraprocedural: each function separately
   ./bin/clam input.bc
   
   # Interprocedural: considers call context
   ./bin/clam --crab-inter input.bc

Interprocedural is more precise but slower.

**Q: Can CLAM verify my assertions?**

A: Yes:

.. code-block:: c

   int main() {
       int x = 5;
       assert(x >= 0 && x <= 10);  // CLAM can verify this
       return 0;
   }

.. code-block:: bash

   ./bin/clam --crab-check=assert input.bc

Bug Detection Questions
~~~~~~~~~~~~~~~~~~~~~~~

**Q: Why does Kint report false positives?**

A: Kint uses bounded SMT-based reasoning and library/function summaries.  A
timeout, incomplete model, or unconstrained input can leave a possible bug
path.  Add assertions or improve models to constrain the relevant behavior:

.. code-block:: c

   void process(int x) {
       if (x < 0 || x > 100) return;
       // Now Kint knows 0 <= x <= 100
       int y = x + 50;  // No overflow warning
   }

**Q: How do I reduce false positives?**

A:

1. Use preprocessing:

.. code-block:: bash

   ./bin/clam-pp --crab-lower-unsigned-icmp input.bc -o prep.bc

2. Use more precise analysis
3. Add annotations/assertions
4. Validate with dynamic analysis (DynAA)

**Q: Can I output results in JSON/SARIF?**

A: Yes:

.. code-block:: bash

   # CLAM JSON output
   ./bin/clam -ojson=results.json input.bc
   
   # Kint with SARIF (via BugReportMgr)
   ./build/bin/lotus-check --engine=kint input.ll --checks=all --report-sarif=results.sarif

Compilation Questions
~~~~~~~~~~~~~~~~~~~~~

**Q: How do I compile multi-file projects?**

A:

.. code-block:: bash

   # Compile each file
   clang -emit-llvm -c file1.c -o file1.bc
   clang -emit-llvm -c file2.c -o file2.bc
   
   # Link into single module
   llvm-link file1.bc file2.bc -o program.bc
   
   # Analyze
   ./bin/tool program.bc

**Q: Should I use .bc or .ll files?**

A:

- ``.bc``: Binary format, faster to load, smaller
- ``.ll``: Text format, human-readable, easier to debug

Both work with Lotus. Use ``.bc`` for production, ``.ll`` for debugging.

**Q: What optimization level should I use?**

A:

.. code-block:: bash

   # For analysis, usually no optimization or -O1
   clang -emit-llvm -c -O0 code.c -o code.bc  # No optimization
   clang -emit-llvm -c -O1 code.c -o code.bc  # Light optimization
   
   # Higher optimization may complicate analysis
   clang -emit-llvm -c -O3 code.c -o code.bc  # Not recommended

Common Error Messages
---------------------

"Module verification failed"
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Error**:

.. code-block:: text

   Module verification failed: <details>

**Solution**: Your LLVM IR is malformed. Verify with:

.. code-block:: bash

   opt -verify input.bc -o /dev/null

Fix the IR or recompile from source.

"Pass 'X' is not initialized"
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Error**:

.. code-block:: text

   Pass 'X' is not initialized

**Solution**: Initialize pass registry:

.. code-block:: cpp

   initializeCore(*PassRegistry::getPassRegistry());

"Couldn't find alias information"
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Error**: Pass expects alias analysis but none provided.

**Solution**: Add alias analysis to pass manager:

.. code-block:: cpp

   PM.add(createBasicAAWrapperPass());
   PM.add(myPass);

"Out of range access" or "Vector index out of bounds"
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Error**: Accessing invalid array/vector index.

**Solution**: This is usually a bug in the analysis. Report it with:

.. code-block:: bash

   # Minimal reproducible example
   clang -emit-llvm -c minimal.c -o minimal.bc
   ./bin/tool minimal.bc

Getting Help
------------

Documentation
~~~~~~~~~~~~~

- :doc:`../index` - Main documentation
- :doc:`architecture` - Framework architecture
- :doc:`../developer/api_reference` - API documentation
- :doc:`tutorials` - Usage examples
- :doc:`../developer/developer_guide` - Extending Lotus

Support Channels
~~~~~~~~~~~~~~~~

1. **GitHub Issues**: https://github.com/ZJU-PL/lotus/issues

   - Bug reports
   - Feature requests
   - Help with errors

2. **GitHub Discussions**: https://github.com/ZJU-PL/lotus/discussions

   - General questions
   - Best practices
   - Show and tell

3. **Stack Overflow**: Tag questions with ``[lotus-analysis]`` and ``[llvm]``

Reporting Bugs
~~~~~~~~~~~~~~

When reporting bugs, include:

1. **Lotus version**: ``git rev-parse HEAD``
2. **LLVM version**: ``llvm-config --version``
3. **System**: OS, compiler version
4. **Input file**: Minimal reproducible example
5. **Command**: Exact command that fails
6. **Output**: Full error message
7. **Backtrace**: If crashed, GDB backtrace

Example bug report:

.. code-block:: text

   **Description**: DyckAA crashes on input file
   
   **Environment**:
   - Lotus: commit abc123def
   - LLVM: 14.0.0
   - OS: Ubuntu 22.04
   - GCC: 11.3.0
   
   **To Reproduce**:
   1. Compile: clang -emit-llvm -c test.c -o test.bc
   2. Run: ./bin/lotus-alias-dyck-aa test.bc
   
   **Error**:
   Segmentation fault (core dumped)
   
   **Backtrace**:
   #0 DyckGraph::addEdge (this=0x0, ...)
   ...
   
   **Input file**: [attach test.c]

Contributing
~~~~~~~~~~~~

See :doc:`../developer/developer_guide` for how to contribute code, documentation, or tests.

See Also
--------

- :doc:`installation` - Installation guide
- :doc:`quickstart` - Quick start guide  
- :doc:`../tools/index` - Tool documentation
