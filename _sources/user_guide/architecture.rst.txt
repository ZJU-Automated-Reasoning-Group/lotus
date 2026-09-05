Architecture Overview
=====================

This document provides a comprehensive overview of the Lotus architecture, describing how the different components interact and how they are organized.

System Architecture
-------------------

Lotus is organized into several major subsystems that work together to provide a comprehensive program analysis framework:

.. code-block:: text

   ┌──────────────────────────────────────────────────────────────┐
   │                      Command-Line Tools                       │
   │  (lotus-alias-aser-aa, lotus-alias-dyck-aa, lotus-check, clam, etc.) │
   └────────────────┬─────────────────────────────────────────────┘
                    │
   ┌────────────────┴─────────────────────────────────────────────┐
   │                    Analysis Applications                      │
   │  - Bug Checkers (AE, FiTx, Kint, Taint, Concurrency,        │
   │                  Pulse, Saber, SymEx, Generic)               │
   │  - Abstract Interpreters (CLAM, SymAbsAI)                               │
   │  - Fuzzing Support (Titan)                                   │
   └────┬────────────┬────────────┬────────────┬─────────────────┘
        │            │            │            │
   ┌────┴────┐  ┌───┴────┐  ┌───┴────┐  ┌────┴─────┐
   │ Alias   │  │  IR    │  │ Data   │  │ Solvers  │
   │ Analysis│  │ (PDG,  │  │ Flow   │  │ (SMT,    │
   │         │  │  VFG)  │  │        │  │  BDD)    │
   └────┬────┘  └───┬────┘  └───┬────┘  └────┬─────┘
        │           │           │            │
   ┌────┴───────────┴───────────┴────────────┴─────────────────┐
   │                    LLVM Infrastructure                      │
   │            (Module, Function, BasicBlock, etc.)            │
   └────────────────────────────────────────────────────────────┘

Core Components
---------------

1. **Alias Analysis** (``lib/Alias/``, ``include/Alias/``)
   
   Multiple pointer analysis algorithms with different precision-performance trade-offs:
   
   - **DyckAA**: Unification-based, exhaustive alias analysis with Dyck-CFL reachability
   - **AserPTA**: Constraint-based pointer analysis with multiple context sensitivities (including k-callsite and k-origin/thread-creation sensitivity)
   - **LotusAA**: Flow-sensitive, field-sensitive interprocedural analysis
   - **Sea-DSA**: Context-sensitive DSA-based memory analysis
   - **SparrowAA**: Inclusion-based pointer analysis
   - **FPA**: Function pointer analysis (FLTA, MLTA, MLTADF, KELP)
   - **SRAA**: Strict Relation Alias Analysis using range analysis
   - **DynAA**: Dynamic validation of static alias analysis results

2. **Intermediate Representations** (``lib/IR/``, ``include/IR/``)
   
   Specialized graph representations for program analysis:
   
   - **PDG (Program Dependence Graph)**: Captures data and control dependencies
   - **DyckVFG (Value Flow Graph)**: Tracks value flow for Dyck-based analyses
   - **Call Graphs**: Multiple call graph implementations (PDG, Dyck, AserPTA)

3. **Data Flow Analysis** (``lib/Dataflow/``, ``include/Dataflow/``)
   
   IFDS/IDE framework for interprocedural data flow problems:
   
   - **Taint Analysis**: Tracks tainted data from sources to sinks
   - **Reaching Definitions**: Identifies definition-use chains
4. **Constraint Solving** (``lib/Solvers/``, ``include/Solvers/``)
   
   Multiple solver backends for different analysis needs:
   
   - **SMT Solver**: Z3-based reasoning for precise constraint solving
   - **BDD Solver**: CUDD-based symbolic set operations
   - **WPDS**: Weighted pushdown systems for interprocedural reachability

5. **Abstract Interpretation** (``third-party/verification/clam/``, ``lib/Verification/SymAbsAI/``)
   
   Numerical and symbolic abstract domains:
   
   - **CLAM**: Abstract interpretation with multiple domains (intervals, zones, octagons, polyhedra)
   - **SymAbsAI**: Configurable abstract interpretation with domain composition

6. **Symbolic Execution** (``lib/SymbolicExecution/``, ``include/SymbolicExecution/``)

   Path-sensitive symbolic execution infrastructure used by the ``symex``
   checker frontend. See :doc:`../symbolic_execution/index` for the engine
   documentation.

   - **AnalysisDriver**: Coordinates whole-module symbolic execution and summaries
   - **AnalysisState**: Tracks symbolic memory, guarded facts, and path conditions
   - **PathCondSolver**: Discharges symbolic path feasibility queries through SMT
   - **SymbolicExecutionWrapper**: Connects the engine to the LLVM pass pipeline

7. **Analysis Utilities** (``lib/Analysis/``, ``include/Analysis/``)
   
   Supporting analysis components:
   
   - **CFG Analysis**: Control flow graph utilities
   - **Concurrency Analysis**: Thread-aware analysis (MHP, lock sets)
   - **Null Pointer Analysis**: Multiple null checking algorithms
   - **Range Analysis**: Value range analysis for integers

8. **Bug Detection** (``lib/Checker/``)
   
   Security and safety bug detection:
   
   - **Kint**: Integer overflow, division by zero, array bounds checking
   - **Taint**: Information flow and taint-style vulnerabilities
   - **Concurrency**: Race conditions and deadlock detection
   - **AE**: Abstract-execution memory-safety checks
   - **FiTx**: Fast typestate-based development feedback
   - **Pulse**: Witness-oriented memory-safety checks
   - **Saber**: Sparse value-flow resource checks
   - **Generic**: Declarative project-specific checks
   - **SymEx**: Symbolic-execution checks through the top-level
     ``SymbolicExecution`` engine

9. **CFL Reachability** (``lib/CFL/``, ``include/CFL/``)
   
   Context-Free Language reachability engines:
   
   - **CSR**: graph indexing for CFL Reachability

Analysis Flow
-------------

Typical Analysis Pipeline
~~~~~~~~~~~~~~~~~~~~~~~~~~

1. **LLVM IR Input**: Start with LLVM bitcode (``.bc``) or IR (``.ll``)

2. **Preprocessing** (Optional):
   
   - Lower unsupported constructs
   - Canonicalize representations
   - Devirtualization
   - Memory operation normalization

3. **Core Analysis**:
   
   - Build call graph
   - Perform alias analysis
   - Construct intermediate representations (PDG, VFG)
   - Run specialized analyses (taint, null pointer, etc.)

4. **Bug Detection/Verification**:
   
   - Apply checkers
   - Validate properties
   - Generate reports

5. **Output**:
   
   - Text reports
   - JSON/SARIF output
   - DOT graphs for visualization

Example: Null Pointer Detection
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: text

   Input: program.bc
      ↓
   [Preprocessing]
      ↓
   [Alias Analysis] → Points-to sets
      ↓
   [Build VFG] → Value flow graph
      ↓
   [Null Flow Analysis] → Track null values
      ↓
   [Check Dereferences] → Identify potential NPDs
      ↓
   Output: Bug report

Pointer Analysis Architectures
-------------------------------

AserPTA Architecture
~~~~~~~~~~~~~~~~~~~~

AserPTA uses a constraint-based approach with multiple solver strategies:

.. code-block:: text

   LLVM IR
      ↓
   [Preprocessing]
      ├─ Canonicalize GEP
      ├─ Lower memcpy
      ├─ Remove exception handlers
      └─ Normalize heap APIs
      ↓
   [Constraint Collection]
      ├─ addr_of: p = &obj
      ├─ copy: p = q
      ├─ load: p = *q
      ├─ store: *p = q
      └─ offset: p = &obj->field
      ↓
   [Constraint Graph]
      ├─ Nodes: Pointers and Objects
      └─ Edges: Constraints
      ↓
   [Solver] (WavePropagation / DeepPropagation)
      ├─ SCC detection
      ├─ Differential propagation
      └─ Cycle elimination
      ↓
   Points-to Sets

LotusAA Architecture
~~~~~~~~~~~~~~~~~~~~

LotusAA provides flow-sensitive, field-sensitive analysis:

.. code-block:: text

   LLVM Module
      ↓
   [Global Analysis]
      ↓
   [Iterative Analysis Loop]
      ├─ [Bottom-Up Traversal]
      │     ├─ Process each function
      │     ├─ Build summaries
      │     └─ Collect interfaces
      │
      ├─ [Call Graph Construction]
      │     ├─ Resolve indirect calls
      │     └─ Update call graph
      │
      └─ [Fixpoint Check]
            ├─ Detect changes
            └─ Reanalyze if needed
      ↓
   Final Points-to Sets + Call Graph

DyckAA Architecture
~~~~~~~~~~~~~~~~~~~

DyckAA uses Dyck-CFL reachability for precise alias analysis:

.. code-block:: text

   LLVM IR
      ↓
   [Build Dyck Graph]
      ├─ Field edges: *[field]*
      ├─ Dereference edges: *(*
      └─ Assignment edges: *()*
      ↓
   [Dyck-CFL Reachability]
      ├─ Compute reachability
      └─ Unify nodes
      ↓
   [Alias Sets]
      ↓
   [Applications]
      ├─ Call graph construction
      ├─ ModRef analysis
      └─ Value flow analysis

Abstract Interpretation Flow (CLAM)
------------------------------------

.. code-block:: text

   LLVM Bitcode
      ↓
   [clam-pp Preprocessing]
      ├─ Lower unsigned comparisons
      ├─ Lower select instructions
      └─ Devirtualization
      ↓
   [CrabIR Translation]
      ├─ Convert LLVM IR to CrabIR
      ├─ Apply memory abstraction
      └─ Integrate heap analysis (Sea-DSA)
      ↓
   [Abstract Interpretation]
      ├─ Select domain (int, zones, oct, pk)
      ├─ Compute fixpoint
      └─ Generate invariants
      ↓
   [Property Checking]
      ├─ Check assertions
      ├─ Check null pointers
      ├─ Check buffer bounds
      └─ Check use-after-free
      ↓
   [Output]
      ├─ Invariants (text/JSON)
      ├─ Verification results
      └─ Warnings/errors

Data Structures
---------------


Constraint Graphs
~~~~~~~~~~~~~~~~~

- **Nodes**: Represent pointers and objects
- **Edges**: Represent constraints (copy, load, store, offset)
- **Super Nodes**: Collapsed SCCs for efficiency

Value Flow Graphs
~~~~~~~~~~~~~~~~~

- **Nodes**: Values and memory locations
- **Edges**: Data flow and pointer propagation
- **Contexts**: Call-site or object sensitivity

Program Dependence Graphs
~~~~~~~~~~~~~~~~~~~~~~~~~~

- **Nodes**: Instructions, functions, parameters
- **Edges**: Data dependencies, control dependencies, parameter bindings
- **Trees**: Hierarchical parameter representations for field sensitivity


Standalone Tools
~~~~~~~~~~~~~~~~

Each major component has standalone command-line tools. The bug-detection
frontends have been unified under a single ``lotus-check`` binary with an
explicit ``--engine=<name>`` selector:

**Alias analysis:**
- ``lotus-alias-aser-aa``, ``lotus-alias-dyck-aa``, ``lotus-alias-lotus-aa``

**Unified checker frontend (``lotus-check``):**
- ``lotus-check --engine=kint`` — Integer overflow, division by zero, array bounds
- ``lotus-check --engine=taint`` — Interprocedural taint analysis
- ``lotus-check --engine=concur`` — Concurrency bug detection (races, deadlocks, OpenMP, MPI)
- ``lotus-check --engine=pulse`` — Biabductive memory-safety analysis
- ``lotus-check --engine=fitx`` — Typestate-based bug detection
- ``lotus-check --engine=saber`` — Source-sink bug detection (leaks, double-free)
- ``lotus-check --engine=ae`` — Abstract-execution-based checking
- ``lotus-check --engine=symex`` — Symbolic-execution bug checking

**PDG query:**
- ``lotus-ir-pdg-query`` — PDG Cypher queries, slicing, chopping, resource-flow analysis

**Abstract interpretation:**
- ``clam``, ``clam-pp``, ``clam-diff``

Python Integration
~~~~~~~~~~~~~~~~~~

Python scripts provide high-level interfaces:

- ``clam.py``: Full analysis pipeline with compilation
- ``clam-yaml.py``: Configuration-based analysis
- ``seapy``: SeaHorn integration

Performance Considerations
--------------------------

Scalability Strategies
~~~~~~~~~~~~~~~~~~~~~~

1. **Context Pruning**: Limit context depth (K in K-CFA)
2. **SCC Collapsing**: Merge strongly connected components
3. **Lazy Evaluation**: Compute results on demand


Extension Points
----------------

Adding New Analyses
~~~~~~~~~~~~~~~~~~~

1. Create LLVM ModulePass or FunctionPass
2. Implement ``runOnModule()`` or ``runOnFunction()``
3. Query existing analyses (AA, PDG, etc.)
4. Register pass in ``CMakeLists.txt``
5. Add tool in ``tools/`` directory

Adding New Checkers
~~~~~~~~~~~~~~~~~~~

1. Extend ``BugDetectorPass`` base class
2. Implement checker logic
3. Report bugs via ``BugReportMgr``
4. Add a ``lotus-check`` engine runner or create a new tool

Adding New Abstract Domains
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

1. Extend CRAB domain interface
2. Implement lattice operations
3. Register with CLAM
4. Add command-line option

See :doc:`../developer/developer_guide` for detailed instructions.
