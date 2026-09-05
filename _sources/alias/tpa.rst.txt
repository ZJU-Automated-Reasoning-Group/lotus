===========================
TPA: Flow- and Contex-Sensitive Pointer Analysis
===========================

Overview
========

TPA is an **inclusion-based**, **flow- and context-sensitive** pointer analysis framework with k-limiting support.
It uses a **semi-sparse** program representation to achieve both precision and scalability for large C/C++ programs.

* **Location**: ``lib/Alias/InclusionBased/TPA``

Architecture
============

The analysis pipeline is organized as:

.. code-block:: text

   LLVM IR
      ↓
   [IR Transforms]
      ├─ Expand GEP instructions
      ├─ Expand constant expressions
      ├─ Expand indirect branches
      ├─ Fold int-to-ptr casts
      └─ Global cleanup
      ↓
   [Front-End Processing]
      ├─ Type collection and analysis
      ├─ Array/pointer layout analysis
      ├─ Struct cast analysis
      ├─ CFG building and simplification
      ├─ Function translation
      └─ Semi-sparse program construction
      ↓
   [Global Pointer Analysis]
      ├─ Initialize global variables
      ├─ Initialize functions
      └─ Set up special pointers (null, universal)
      ↓
   [Semi-Sparse Analysis]
      ├─ Worklist-based propagation
      ├─ Transfer function evaluation
      ├─ Context management
      └─ Store pruning
      ↓
   Points-to Sets (Env and Store)

Core Components
===============

1. **Front-End** (``FrontEnd/``)
   - **TypeCollector**: Collects and analyzes types from the LLVM module
   - **TypeAnalysis**: Performs type-based analysis
   - **ArrayLayoutAnalysis**: Analyzes array memory layouts
   - **PointerLayoutAnalysis**: Analyzes pointer memory layouts
   - **StructCastAnalysis**: Handles structure casting
   - **CFGBuilder**: Builds control-flow graphs from LLVM functions
   - **CFGSimplifier**: Simplifies CFGs for efficiency
   - **FunctionTranslator**: Translates LLVM functions to CFG representation
   - **InstructionTranslator**: Translates LLVM instructions to CFG nodes
   - **SemiSparseProgramBuilder**: Constructs the semi-sparse program representation
   - **PriorityAssigner**: Assigns priorities to CFG nodes

2. **Program Representation** (``Program/``)
   - **SemiSparseProgram**: Main program representation containing CFGs
   - **CFG**: Control-flow graph with nodes for different instruction types
   - **CFGNode**: Base class for CFG nodes (Entry, Alloc, Copy, Offset, Load, Store, Call, Return)

3. **Memory Model** (``MemoryModel/``)
   - **MemoryManager**: Manages memory objects and allocations
   - **PointerManager**: Manages pointer values and their contexts
   - **TypeLayout**: Represents type layouts for memory modeling
   - **ArrayLayout**: Handles array memory layouts
   - **PointerLayout**: Handles pointer memory layouts

4. **Analysis Engine** (``Engine/``)
   - **SemiSparsePointerAnalysis**: Main analysis class
   - **GlobalPointerAnalysis**: Initializes global state
   - **TransferFunction**: Evaluates transfer functions for CFG nodes
   - **SemiSparsePropagator**: Propagates facts through the program
   - **Initializer**: Sets up initial analysis state
   - **StorePruner**: Prunes unnecessary store information
   - **ExternalCallAnalysis**: Handles external function calls
   - **Transfer/**: Specialized transfer functions:
     - **AllocTransfer**: Memory allocation
     - **CopyTransfer**: Pointer assignments
     - **OffsetTransfer**: Field/array offset operations
     - **LoadTransfer**: Pointer dereferences (loads)
     - **StoreTransfer**: Pointer stores
     - **CallTransfer**: Function calls
     - **ReturnTransfer**: Function returns
     - **EntryTransfer**: Function entry points

5. **Context Management** (``Context/``)
   - **Context**: Base context representation (call stack)
   - **KLimitContext**: K-limiting context sensitivity (limits call stack depth)
   - **AdaptiveContext**: Adaptive context sensitivity (selective tracking)

6. **Precision Tracking** (``Precision/``)
   - **PrecisionLossTracker**: Tracks precision loss during analysis
   - **ValueDependenceTracker**: Tracks value dependencies

7. **Output** (``Output/``)
   - **CFGNodePrinter**: Prints CFG nodes
   - **ContextPrinter**: Prints contexts
   - **MemoryPrinter**: Prints memory objects
   - **NodePrinter**: General node printing
   - **WriteDotFile**: Writes DOT graph files

Transfer Functions
==================

TPA uses transfer functions to model the effect of each CFG node on the
points-to information:

1. **Entry**: Initialize function parameters
2. **Alloc**: Create new memory objects (heap/stack allocations)
3. **Copy**: Pointer assignment (``p = q``)
4. **Offset**: Field/array access (``p = &obj->field`` or ``p = &arr[i]``)
5. **Load**: Pointer dereference (``p = *q``)
6. **Store**: Store through pointer (``*p = q``)
7. **Call**: Function call handling with context sensitivity
8. **Return**: Return value propagation

Context Sensitivity
===================

TPA supports multiple context sensitivity models:

1. **Basic Context** (``Context``)
   - Full call stack tracking
   - Unbounded context depth
   - Most precise but potentially expensive

2. **K-Limited Context** (``KLimitContext``)
   - Limits call stack depth to K
   - Configurable limit (default can be set)
   - Balances precision and scalability

3. **Adaptive Context** (``AdaptiveContext``)
   - Selectively tracks important call sites
   - Reduces context explosion for less critical paths
   - Configurable tracking policy

Memory Model
============

TPA uses a field-sensitive memory model:

- **Type-based Layout**: Memory objects are organized by type
- **Array Layout**: Arrays are modeled with element-level precision
- **Pointer Layout**: Pointer fields are tracked separately
- **Struct Layout**: Structure fields are modeled individually

The memory model distinguishes between:
- **Top-level pointers**: Variables and function parameters
- **Memory objects**: Heap allocations, stack allocations, globals
- **Store**: Maps memory objects to their points-to sets

Semi-Sparse Representation
===========================

The semi-sparse representation reduces the number of program points that need
to be analyzed:

- Only **def-use** chains are tracked (not all program points)
- CFG nodes are classified by their effect on pointer information
- The worklist algorithm processes only relevant nodes
- Store pruning removes redundant information

Analysis Algorithm
==================

The analysis follows this workflow:

1. **Initialization**:
   - Build semi-sparse program from LLVM IR
   - Initialize global variables and functions
   - Set up special pointers (null, universal)

2. **Worklist Processing**:
   - Start from entry function with initial store
   - For each program point in worklist:
     - Evaluate transfer function
     - Update points-to information
     - Propagate to successors
     - Add changed successors to worklist

3. **Context Handling**:
   - At call sites: push new context
   - At returns: pop context and merge results
   - K-limiting or adaptive strategies control context growth

4. **Convergence**:
   - Analysis terminates when worklist is empty
   - Memoization prevents redundant computations

Usage
=====

TPA is typically used through the Lotus alias analysis wrapper. It can be
configured via:

* **Context sensitivity mode**: Basic, K-limited, or adaptive
* **K-limit value**: For K-limited context sensitivity
* **External pointer table**: For modeling external library functions
* **Precision tracking**: Enable precision loss tracking

Integration
===========

TPA integrates with other Lotus components:

* **Annotation System**: Uses external pointer annotations for library functions
* **Alias Analysis Wrapper**: Provides unified interface for alias queries
* **Type System**: Leverages LLVM type information for memory modeling

The analysis results (points-to sets) can be queried through the
``PointerAnalysis`` base class interface, which provides methods to:
- Get points-to sets for pointers
- Resolve indirect call targets
- Query alias relationships
