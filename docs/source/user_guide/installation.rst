Installation Guide
===================

Installation guide for Lotus and its dependencies.

Prerequisites
-------------

* LLVM 14.0.0
* Z3 4.11
* CMake 3.10+
* C++17 compatible compiler
* Boost 1.65+ (optional — only needed when CLAM, SeaHorn, Cclyzer++, or FPsolve are enabled;
  auto-downloaded if not found)

Building Lotus
--------------

.. code-block:: bash

   git clone https://github.com/ZJU-PL/lotus
   cd lotus
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
   cmake --build build -j

.. note::

   After configuration, CMake prints a **build summary** showing all enabled
   features. Run ``cmake -S . -B build`` (no flags) for a quick overview.

Configuration Options
---------------------

Lotus exposes its project-owned CMake toggles through ``cmake/LotusOptions.cmake``.
Options use a consistent ``LOTUS_*`` naming scheme.

**Core toggles:**

* ``-DLLVM_BUILD_PATH=/path/to/llvm/lib/cmake/llvm``: Only needed if CMake cannot
  find a supported LLVM automatically.
* ``-DLOTUS_BUILD_TESTS=ON``: Build unit tests (default: OFF)
* ``-DLOTUS_BUILD_EXAMPLES=ON``: Build examples (default: OFF)
* ``-DLOTUS_ENABLE_COVERAGE=ON``: Instrument Lotus and its tests for LLVM source
  coverage (requires ``LOTUS_BUILD_TESTS=ON`` and Clang/AppleClang; default: OFF)

**Optional verifier integrations** (all OFF by default — opt-in due to heavyweight dependencies):

* ``-DLOTUS_ENABLE_CLAM=ON``: Enable CLAM abstract interpretation framework
* ``-DLOTUS_ENABLE_SEAHORN=ON``: Enable SeaHorn
* ``-DLOTUS_ENABLE_SMACK=ON``: Enable SMACK LLVM-to-Boogie verifier frontend
* ``-DLOTUS_ENABLE_SVF=ON``: Enable SVF
* ``-DLOTUS_ENABLE_HORN_ICE=ON``: Build ICE learning tools for CHC
* ``-DLOTUS_ENABLE_SEAL=ON``: Build the Seal symbolic automata lifter (CAV 2026)

**Optional in-tree tool families** (all OFF by default):

* ``-DLOTUS_ENABLE_CFL=ON``: Build CFL reachability solvers
* ``-DLOTUS_ENABLE_CSR=ON``: Build the indexing context-sensitive reachability solver
* ``-DLOTUS_ENABLE_OWL=ON``: Build the Owl SMT solver
* ``-DLOTUS_ENABLE_SMT_STABILIZER=ON``: Build the SMTStabilizer SMT-LIB
  normalization library and tool (needs GMP, GMPXX, and MPFR)
* ``-DLOTUS_ENABLE_DYNAA=ON``: Build dynamic alias-analysis tools
* ``-DLOTUS_USE_CCLYZER=ON``: Enable optional cclyzer++ alias analysis backend
* ``-DLOTUS_ENABLE_TYPE_QUALIFIER=ON``: Enable the TypeQualifier uninitialized-data checker
* ``-DLOTUS_ENABLE_FPSOLVE=ON``: Build vendored FPsolve fixed-point solver
* ``-DLOTUS_ENABLE_WALI_OPENNWA=ON``: Build vendored WALi/OpenNWA weighted automata

**Advanced toggles:**

* ``-DLOTUS_DOWNLOAD_BOOST=OFF``: Disable Boost auto-download (default: ON)
* ``-DLOTUS_DOWNLOAD_CRAB=ON``: Allow CRAB auto-download (default: OFF)
* ``-DLOTUS_CUSTOM_BOOST_ROOT=/path/to/boost``: Path to a custom Boost installation
* ``-DLOTUS_CUSTOM_CRAB_ROOT=/path/to/crab``: Path to a custom CRAB checkout
* ``-DLOTUS_SEADSA_ENABLE_SANITY_CHECKS=ON``: Enable Sea-DSA sanity checks (default: OFF)
* ``-DLOTUS_WPDS_WITNESS_TRACE=ON``: Enable WPDS witness tracing (default: OFF)
* ``-DLOTUS_SEAHORN_BUILD_32_BIT_RT=ON``: Build 32-bit SeaHorn runtime libraries (default: OFF)
* ``-DLOTUS_EGRAPH_ENABLE_DOT=ON``: Enable DOT/Graphviz helpers in the EGraph library (default: ON)
* ``-DLOTUS_EGRAPH_ENABLE_JSON=ON``: Enable JSON serialization helpers in the EGraph library (default: ON)

Typical configurations:

.. code-block:: bash

   # Lean local build (the defaults exclude tests and CLAM)
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug

   # Full build, including tests and CLAM
   cmake -S . -B build-full -G Ninja \
     -DLOTUS_BUILD_TESTS=ON \
     -DLOTUS_ENABLE_CLAM=ON

   # Enable optional tool families
   cmake -S . -B build \
     -DLOTUS_ENABLE_DYNAA=ON \
     -DLOTUS_ENABLE_HORN_ICE=ON \
     -DLOTUS_ENABLE_CFL=ON \
     -DLOTUS_ENABLE_CSR=ON

   # Enable Seal symbolic automata lifter
   cmake -S . -B build -DLOTUS_ENABLE_SEAL=ON

Z3 Installation
---------------

.. code-block:: bash

   # Ubuntu/Debian
   sudo apt-get install libz3-dev

   # macOS with Homebrew
   brew install z3

   # Or build from source
   git clone https://github.com/Z3Prover/z3.git
   cd z3 && python scripts/mk_make.py
   cd build && make && sudo make install


Troubleshooting
---------------

* **LLVM not found**: Install LLVM 14.x via your package manager (or from source).
  If you use a non-standard installation location, set ``LLVM_BUILD_PATH`` to the directory
  that contains ``LLVMConfig.cmake`` and re-run CMake.
* **Z3 not found**: Install Z3 or set ``Z3_DIR``
* **Boost issues**: Use ``LOTUS_CUSTOM_BOOST_ROOT`` or let Lotus auto-download Boost
* **Build errors**: Use supported LLVM version (14.x)
