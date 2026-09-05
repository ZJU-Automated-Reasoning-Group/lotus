SVFG — Sparse Value-Flow Graph
===============================

Overview
========

**Sparse Value-Flow Graph (SVFG)** is a sparse representation of value flows
across a whole program, enabling efficient interprocedural analysis. The SVFG
combines Static Single Assignment (SSA) form with Memory SSA to track both
top-level pointers (address-taken variables) and their value flows through
memory operations.

This sparse MemorySSA is constructed by the SVFG builder using AserPTA. It is
independent of :doc:`shadowmemssa`, which queries Sea-DSA ``shadow.mem.*``
instrumentation for interprocedural optimizations.

* **Location**: ``lib/IR/SVFG/``, ``include/IR/SVFG/``
* **Migrated from**: SVF (SVF-master, https://github.com/SVF-tools/SVF)

The SVFG provides a unified graph structure where:

* **Top-level pointers** (SSA variables) flow through copy, GEP, and arithmetic operations.
* **Address-taken variables** (heap, stack, globals) flow through load/store via Memory SSA.
* **Interprocedural value flow** is captured through parameter passing and return values.

Key Features
============

* **Sparse Value-Flow Representation**: Only defines and uses are connected, avoiding
  exhaustive memory modeling.
* **Memory SSA Integration**: Tracks value flows through memory via Mu (use) and Chi (def) nodes.
* **Points-To Set Association**: Indirect edges carry points-to sets for precise alias analysis.
* **Interprocedural Flow**: Complete call/return edges connecting actual and formal parameters.
* **On-the-Fly Call Graph Refinement**: Supports demand-driven indirect call resolution.
* **AserPTA Integration**: Uses AserPTA for field-sensitive pointer analysis.

Node Types
==========

Statement Nodes (Top-Level Pointers)
-------------------------------------

These nodes represent operations on top-level pointers (SSA variables):

* **AddrSVFGNode**: Address-taking instructions (alloca, malloc, address-of).
  Creates an abstract object and defines a pointer to it.
* **CopySVFGNode**: Copy operations (bitcast, inttoptr, ptrtoint).
* **LoadSVFGNode**: Load instructions. Reads from memory through a pointer.
* **StoreSVFGNode**: Store instructions. Writes to memory through a pointer.
* **GepSVFGNode**: GetElementPtr instructions. Computes derived pointers.
* **BinaryOpSVFGNode**: Binary operations on pointer values.
* **CmpSVFGNode**: Comparison instructions involving pointers.
* **BranchSVFGNode**: Branch instructions with pointer-dependent conditions.

PHI Nodes
---------

* **IntraPhiSVFGNode**: Intra-procedural PHI nodes for merging values at control-flow joins.
* **InterPhiSVFGNode**: Inter-procedural PHI nodes for parameter/return value aggregation.

Memory SSA Nodes (Address-Taken Variables)
------------------------------------------

These nodes track value flows through memory locations:

* **FormalInSVFGNode**: Memory state at function entry (formal parameter memory).
* **FormalOutSVFGNode**: Memory state at function exit (formal return memory).
* **ActualInSVFGNode**: Memory state before call site (actual parameter memory).
* **ActualOutSVFGNode**: Memory state after call site (actual return memory).
* **LoadMuSVFGNode**: Memory use (load from address-taken location).
* **StoreChiSVFGNode**: Memory def (store to address-taken location).
* **CallMuSVFGNode**: Memory use at call site (for functions that may read memory).
* **CallChiSVFGNode**: Memory def at call site (for functions that may write memory).
* **RetMuSVFGNode**: Memory use at function return.
* **EntryChiSVFGNode**: Memory def at function entry.

Memory PHI Nodes
~~~~~~~~~~~~~~~~

* **IntraMSSAPhiSVFGNode**: Intra-procedural memory PHI for merging memory states.
* **InterMSSAPhiSVFGNode**: Inter-procedural memory PHI node kind (currently not
  materialized by default builder construction).

Parameter Nodes
---------------

* **FormalParmSVFGNode**: Formal parameter at function entry.
* **ActualParmSVFGNode**: Actual parameter at call site.
* **FormalRetSVFGNode**: Formal return value at function exit.
* **ActualRetSVFGNode**: Actual return value at call site.

Edge Types
==========

Intra-Procedural Edges
----------------------

* **IntraDirect**: Direct value flow (copy, bitcast).
* **IntraCopy**: Copy of pointer value.
* **IntraLoad**: Load through pointer (defines loaded value).
* **IntraStore**: Store through pointer (uses stored value).
* **IntraGep**: GEP operation (derived pointer).
* **IntraPhi**: PHI node value flow.
* **IntraMu**: Memory use edge (LoadMu).
* **IntraChi**: Memory def edge (StoreChi).
* **IntraIndirect**: Indirect value flow with points-to guard.

Inter-Procedural Edges
----------------------

* **CallDir**: Direct call parameter passing.
* **CallInd**: Indirect call parameter passing (with points-to guard).
* **CallAIn**: Actual-in to formal-in edge (memory).
* **CallFIn**: Formal-in edge (memory).
* **RetDir**: Direct return value flow.
* **RetInd**: Indirect return value flow (with points-to guard).
* **RetAOut**: Actual-out to formal-out edge (memory).
* **RetFOut**: Formal-out edge (memory).
* **ParamCall**: Parameter passing edge without callsite context.
* **ParamRet**: Return value edge without callsite context.

Thread Edges
------------

* **ThreadMHPIndirectVF**: May-happen-in-parallel indirect value flow for
  concurrent program analysis.

Architecture
============

The SVFG builder follows a phased construction approach:

1. **Pointer Analysis Phase**

   Run AserPTA to compute points-to sets for all pointers:

   * Field-sensitive or field-insensitive memory model.
   * Andersen's algorithm, Wave Propagation, or Deep Propagation.
   * On-the-fly call graph construction for indirect calls.

2. **Node Construction Phase**

   Create SVFG nodes for all program values:

   * Top-level pointer nodes (Addr, Copy, Load, Store, Gep, etc.).
   * Parameter nodes (FormalParm, ActualParm, FormalRet, ActualRet).
   * Memory SSA nodes (FormalIn/Out, ActualIn/Out, LoadMu, StoreChi).

3. **Edge Construction Phase**

   Connect nodes based on value flow:

   * Intra-procedural direct edges.
   * Intra-procedural indirect edges (with points-to sets).
   * Inter-procedural call/return edges.

4. **Memory SSA Phase**

   Build memory SSA form for address-taken variables:

   * Memory region identification from points-to sets.
   * SSA versioning for memory locations.
   * Memory PHI nodes at control-flow merges.
   * Interprocedural memory flow through call/return edges
     (``CallAIn``/``RetAOut``).

Usage
=====

Basic Construction
------------------

.. code-block:: cpp

   #include "IR/SVFG/SVFG.h"
   #include "IR/SVFG/SVFGBuilder.h"
   
   // Build SVFG from ICFG
   SVFGBuilder builder;
   SVFG* svfg = builder.build(icfg);
   
   // Iterate over nodes
   for (auto& pair : *svfg) {
       SVFGNode* node = pair.second;
       // Process node...
   }

Configuration
-------------

.. code-block:: cpp

   #include "IR/SVFG/SVFGBuilder.h"
   
   SVFGBuilderConfig config;
   config.buildMSSA = true;
   config.usePointerAnalysis = true;
   config.resolveIndirectCalls = true;
   config.solverType = SVFGBuilderConfig::SolverType::Andersen;
   config.memModelType = SVFGBuilderConfig::MemModelType::FieldSensitive;
   
   SVFGBuilder builder(config);
   SVFG* svfg = builder.build(icfg);

Querying Value Flows
--------------------

.. code-block:: cpp

   #include "IR/SVFG/SVFG.h"
   
   // Get definition site for an instruction
   SVFGNode* def = svfg->getDef(instruction);
   
   // Get successors (forward reachability)
   SVFGNodeSet succs = svfg->getSuccs(node);
   
   // Get predecessors (backward reachability)
   SVFGNodeSet preds = svfg->getPreds(node);
   
   // Check path existence
   bool hasPath = svfg->hasPath(srcNode, dstNode);

Memory SSA Queries
------------------

.. code-block:: cpp

   #include "IR/SVFG/SVFG.h"
   
   // Get LoadMu nodes for a load instruction
   const SVFGNodeSet& mus = svfg->getLoadMus(loadInst);
   
   // Get StoreChi nodes for a store instruction
   const SVFGNodeSet& chis = svfg->getStoreChis(storeInst);
   
   // Get points-to set for a memory node
   const SVFGNodeBS* pts = memNode->getPointsTo();

Interprocedural Queries
-----------------------

.. code-block:: cpp

   #include "IR/SVFG/SVFG.h"
   
   // Get formal parameters for a function
   const SVFGNodeSet& formalParms = svfg->getFormalParms(func);
   
   // Get actual parameters for a call site
   const SVFGNodeSet& actualParms = svfg->getActualParms(callSite);
   
   // Get formal return node
   const SVFGNodeSet& formalRets = svfg->getFormalRets(func);
   
   // Get actual return node
   const SVFGNodeSet& actualRets = svfg->getActualRets(callSite);

On-the-Fly Call Graph Refinement
--------------------------------

.. code-block:: cpp

   #include "IR/SVFG/SVFGBuilder.h"
   
   // Configure SVFG to defer indirect call resolution
   SVFGBuilderConfig config;
   config.resolveIndirectCalls = false;
   
   SVFGBuilder builder(config);
   SVFG* svfg = builder.build(icfg);
   
   // Later, when DDA discovers an indirect call target:
   std::vector<SVFGEdge*> newEdges;
   builder.connectCallSiteToCalleeOnTheFly(svfg, callSite, callee, newEdges);

Type-Safe Node Casting
----------------------

.. code-block:: cpp

   #include "IR/SVFG/SVFGNode.h"
   
   SVFGNode* node = ...;
   
   if (auto* addrNode = llvm::dyn_cast<AddrSVFGNode>(node)) {
       uint32_t objId = addrNode->getObjectId();
   }
   
   if (auto* loadNode = llvm::dyn_cast<LoadSVFGNode>(node)) {
       uint32_t ptrId = loadNode->getLoadFromPtr();
   }
   
   if (auto* mssaNode = llvm::dyn_cast<MSSASVFGNode>(node)) {
       uint32_t memReg = mssaNode->getMemReg();
       uint32_t version = mssaNode->getSSAVersion();
       const SVFGNodeBS* pts = mssaNode->getPointsTo();
   }

Serialization
-------------

.. code-block:: cpp

   #include "IR/SVFG/SVFG.h"
   
   // Write to file
   svfg->writeToFile("output.svfg");
   
   // Read from file
   SVFG* loaded = new SVFG();
   loaded->readFromFile("output.svfg");
   
   // Dump to DOT format
   svfg->dump("svfg.dot");

Integration
===========

AserPTA Integration
-------------------

The SVFG builder uses **AserPTA** as its pointer analysis engine:

* **Location**: ``lib/Alias/InclusionBased/AserPTA/``
* **Solvers**: Andersen, WavePropagation, DeepPropagation, PartialUpdate
* **Memory Models**: Field-sensitive (FSMemModel), Field-insensitive (FIMemModel)
* **Context Sensitivity**: Context-insensitive (NoCtx), k-call-site sensitive (KCallSite<N>)

ICFG Integration
----------------

The SVFG is built on top of the Interprocedural Control-Flow Graph:

* Each SVFG node is associated with an ICFGNode.
* Call sites and function entries/exits are derived from ICFG.

DDA Integration
---------------

The Demand-Driven Alias Analysis (DDA) uses SVFG for:

* **Value-flow backtracing**: Follow SVFG edges backward to find definitions.
* **Indirect call resolution**: On-the-fly edge creation for discovered callees.
* **Object tracking**: Points-to sets on edges enable precise alias reasoning.

Components
==========

Core Files
----------

* **SVFG.h/cpp**: Main graph class with node/edge management.
* **SVFGBase.h**: Node/edge kind enumerations and type-checking helpers.
* **SVFGNode.h**: Complete node type hierarchy.
* **SVFGEdge.h**: Complete edge type hierarchy.
* **SVFGBuilder.h/cpp**: Builder with AserPTA integration.
* **SVFGBuilderNodes.cpp**: Node construction logic.
* **SVFGBuilderEdges.cpp**: Edge construction logic.
* **SVFGBuilderMemorySSA.cpp**: Memory SSA construction.
* **SVFGOPT.h/cpp**: Optimization passes (dead node elimination, etc.).
* **SVFGSerializer.h/cpp**: Serialization support.
* **SVFGStats.h/cpp**: Statistics collection.
* **GraphTraits.h**: LLVM graph traits for traversal algorithms.
* **PointsToSetHash.h**: Hash functions for points-to sets.

Dependencies
------------

* **CanaryICFG**: Control-flow graph infrastructure.
* **AserPTA**: Pointer analysis engine.
* **LLVM Core/Support**: LLVM IR and utilities.

References
==========

* Yulei Sui, Ding Ye, Jingling Xue. "Detecting Memory Leaks Statically with
  Full-Sparse Value-Flow Analysis". IEEE Transactions on Software Engineering
  (TSE), 2014.

* Yulei Sui, Jingling Xue. "On-Demand Strong Update Analysis via Value-Flow
  Refinement". ACM SIGSOFT International Symposium on Foundations of Software
  Engineering (FSE), 2016.


* SVF Framework: https://github.com/SVF-tools/SVF
