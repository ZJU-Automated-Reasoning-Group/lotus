SymAbsAI – Symbolic Abstraction + Abstract Interpretation
==========================================================

SymAbsAI is a framework for static program analysis using symbolic abstraction
to provide a flexible interface for designing program analyses in a
compositional way.

**Binary**: ``lotus-verify-symabs-ai``  
**Source**: ``tools/verifier/symabs-ai/lotus-verify-symabs-ai.cpp``

For the underlying framework and API documentation, see :doc:`../../../verification/symabs-ai`.

Overview
--------

SymAbsAI provides a command-line tool for running abstract interpretation
analyses on LLVM bitcode. It supports multiple abstract domains and fragment
decomposition strategies for scalable analysis.

**Key features**:

- Multiple abstract domains: intervals, numerical relations, constants, memory regions
- Configurable fragment strategies for balancing precision and performance
- Configuration file support for reproducible analyses
- Integration with Z3 for symbolic reasoning

Quick Start
-----------

**Basic usage**:

.. code-block:: bash

   # Analyze all functions
   ./build/bin/lotus-verify-symabs-ai example.bc

   # Analyze specific function
   ./build/bin/lotus-verify-symabs-ai --function=foo example.bc

   # Use a configuration file
   ./build/bin/lotus-verify-symabs-ai --config config/symabs-ai/01_const_function.conf example.bc

**List available options**:

.. code-block:: bash

   # List available abstract domains
   ./build/bin/lotus-verify-symabs-ai --list-domains

   # List available configuration files
   ./build/bin/lotus-verify-symabs-ai --list-configs

   # List functions in a module
   ./build/bin/lotus-verify-symabs-ai --list-functions example.bc

Configuration Files
-------------------

SymAbsAI uses configuration files (``.conf``) to specify analysis parameters.
Configuration files are located in ``config/symabs-ai/``.

**Example configuration**:

.. code-block:: ini

   # Fragment decomposition strategy
   FragmentDecomposition.Strategy = Edges

   # Abstract domains (comma-separated for product domains)
   AbstractDomain = SimpleConstProp, Interval

   # Memory model variant
   MemoryModel.Variant = BlockModel

   # Analyzer settings
   Analyzer.WideningDelay = 20

   # Verbose output
   SymAbsAIPass.Verbose = false

**Common configurations**:

- ``01_const_function.conf`` – Fast constant propagation (whole function)
- ``03_const_edge.conf`` – Balanced constant propagation (loop-friendly)
- ``05_const_rel_edge.conf`` – Constants + numerical relations
- ``memsafety_sv16_body.conf`` – Memory safety analysis (SV-COMP style)

Command-Line Options
--------------------

**Domain selection**:

- ``--abstract-domain <NAME>`` – Override domain from config file
- ``--list-domains`` – Show available abstract domains

**Function selection**:

- ``--function <NAME>`` – Analyze specific function only
- ``--list-functions`` – List all functions in the module

**Configuration**:

- ``--config <FILE>`` – Use configuration file
- ``--list-configs`` – List available configuration files
- ``SYMBOLIC_ABSTRACTION_CONFIG=<FILE>`` – Environment variable for default config

**Output**:

- ``--verbose`` – Enable detailed output
- ``--dump-cfg`` – Dump control flow graph

Abstract Domains
----------------

Available abstract domains include:

- **SimpleConstProp** – Constant propagation
- **Interval** – Value range analysis
- **NumRels** – Numerical relations (e.g., ``x <= y + 5``)
- **Affine** – Affine relationships
- **BitMask** – Bit-level tracking
- **MemRange** – Memory access bounds
- **MemRegions** – Memory region analysis

Multiple domains can be combined using product domains (comma-separated in config).

Fragment Strategies
-------------------

Fragment decomposition strategies control where abstraction points are placed:

- **Function** – Analyze whole function as one fragment (fastest, least precise)
- **Edges** – Abstract after every basic block (most precise, slowest)
- **Headers** – Abstract at loop headers (good balance)
- **Body** – Abstract in loop bodies
- **Backedges** – Abstract at loop backedges

For more details on the framework architecture and API, see :doc:`../../../verification/symabs-ai`.
