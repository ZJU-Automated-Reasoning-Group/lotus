Stingx — String Constraint Solver
==================================

Stingx is a string constraint solver backend for invariant generation. It is
vendored in ``third-party/stingx_toolchain/`` and used by experimental
invariant-generation scripts in this repository.

**Location**: ``third-party/stingx_toolchain/`` (vendored, local use)

Overview
--------

Stingx solves systems of linear constraints over string variables and generates
loop invariants expressed as linear arithmetic constraints. It parses
constraint descriptions from ``.in`` files and produces
invariant candidates.

The toolchain includes:

- A lexer/parser (``LinTS.l`` / ``LinTS.y``) for the constraint input format.
- Core constraint-solving engine with expression stores, matrix solvers, and
  polynomial stores.
- A standalone driver (``main.cpp``) that reads ``.in`` files and runs the
  invariant-generation pipeline.

Directory Structure
-------------------

::

   third-party/stingx_toolchain/
   ├── main.cpp              # Standalone driver
   ├── CMakeLists.txt        # Local build entry
   ├── Dockerfile            # Container build environment
   └── stingx/               # Vendored backend
       ├── Expression.*      # Expression representation
       ├── LinTS.*           # Linear constraint system
       ├── Elimination.*     # Variable elimination
       ├── MatrixStore.*     # Matrix storage
       ├── PolyStore.*       # Polynomial store
       └── ...

Build and Run
-------------

.. code-block:: bash

   cd third-party/stingx_toolchain
   cmake -S . -B build
   cmake --build build -j$(nproc)
   ./build/bin/stingx_file_runner <input.in>
