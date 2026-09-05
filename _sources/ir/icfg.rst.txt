ICFG — Interprocedural Control Flow Graph
=========================================

Overview
========

The **Interprocedural Control Flow Graph (ICFG)** is the primary control-flow
representation used by many analyses in Lotus.
It extends the standard intraprocedural CFG with **call** and **return**
edges to capture interprocedural control flow.

* **Location**: ``lib/IR/ICFG/``, ``include/IR/ICFG/``

The ICFG serves as a backbone graph on top of which many of the dataflow
engines in Lotus operate. It provides a unified view of control flow across
function boundaries, enabling whole-program analysis.

Key Features
============

* **Interprocedural Edges**: Call and return edges connecting caller and callee code
* **Node Types**: Support for intraprocedural blocks, function entry points, and return points
* **Graph Analysis Utilities**: Back edge detection, reachability queries, and shortest path computation
* **Cycle Removal**: Optional removal of intraprocedural and interprocedural cycles for acyclic analysis
* **Call Graph Integration**: Built-in support for call graph construction and traversal
* **Context-Aware Traversals**: Support for context-sensitive traversals used by dataflow and reachability engines
* **Integration**: Seamless integration with higher-level analyses such as IFDS/IDE, WPDS, and PDG construction

Components
==========

**ICFG** (``ICFG.h``, ``ICFG.cpp``):

The main graph class that extends ``GenericGraph<ICFGNode, ICFGEdge>``. It provides:

* Node and edge management (add, remove, query)
* Intraprocedural and interprocedural edge operations
* Mapping from LLVM basic blocks and functions to ICFG nodes
* Graph traversal and iteration interfaces

**ICFGNode** (``ICFGNode.h``):

Base class for ICFG nodes with three types:

* ``IntraBlock`` – Represents a basic block within a function
* ``FunEntryBlock`` – Represents a function entry point (currently unused)
* ``FunRetBlock`` – Represents a function return point (currently unused)

Each node maintains references to its parent function and basic block, and provides
string representation for debugging.

**ICFGEdge** (``ICFGEdge.h``):

Represents control flow connections between ICFG nodes with three edge kinds:

* ``IntraCF`` – Intraprocedural control flow (within a single function)
* ``CallCF`` – Call edge (from caller to callee entry)
* ``RetCF`` – Return edge (from callee exit back to caller)

**ICFGBuilder** (``ICFGBuilder.h``, ``ICFGBuilder.cpp``):

Constructs an ICFG from an LLVM module by:

* Processing all functions to create intraprocedural nodes and edges
* Adding call edges from call sites to callee entry points
* Adding return edges from callee exits back to call sites
* Optionally removing cycles (both intraprocedural loops and interprocedural recursion)

**LTCallGraph** (``CallGraph.h``, ``CallGraph.cpp``):

A custom call graph implementation that:

* Builds explicit (direct) call relationships between functions
* Omits address-taken functions for more precise analysis
* Maintains call records mapping call instructions to callee functions
* Provides iteration and query interfaces for call graph traversal

**GraphAnalysis** (``GraphAnalysis.h``, ``GraphAnalysis.cpp``):

Utility functions for graph analysis:

* **Back Edge Detection**: Finds intraprocedural and interprocedural back edges
* **Reachability**: Checks if one basic block can reach another
* **Shortest Paths**: Computes shortest paths between nodes
* **Distance Maps**: Calculates shortest distances from a source node

Usage
=====

**Building an ICFG**:

.. code-block:: cpp

   #include "IR/ICFG/ICFG.h"
   #include "IR/ICFG/ICFGBuilder.h"
   
   // Create an empty ICFG
   ICFG* icfg = new ICFG();
   
   // Build ICFG from LLVM module
   ICFGBuilder builder(icfg);
   builder.setRemoveCycleAfterBuild(true);  // Optional: remove cycles
   builder.build(module);
   
   // Access nodes and edges
   ICFGNode* node = icfg->getICFGNode(nodeId);
   ICFGEdge* edge = icfg->getICFGEdge(srcNode, dstNode, ICFGEdge::IntraCF);

**Traversing the ICFG**:

.. code-block:: cpp

   // Iterate over all nodes
   for (auto it = icfg->begin(); it != icfg->end(); ++it) {
       ICFGNode* node = it->second;
       const llvm::Function* func = node->getFunction();
       const llvm::BasicBlock* bb = node->getBasicBlock();
   }
   
   // Iterate over outgoing edges
   for (auto* edge : node->getOutEdges()) {
       ICFGNode* dst = edge->getDstNode();
       if (edge->isCallCFGEdge()) {
           // Handle call edge
       } else if (edge->isRetCFGEdge()) {
           // Handle return edge
       }
   }

**Using Graph Analysis Utilities**:

.. code-block:: cpp

   #include "IR/ICFG/GraphAnalysis.h"
   
   // Find back edges in a function
   std::set<ICFGEdge*> backEdges;
   findFunctionBackedgesIntraICFG(icfg, func, backEdges);
   
   // Compute shortest distances
   std::map<ICFGNode*, uint64_t> distances;
   calculateDistanceMapInterICFGWithDistanceMap(icfg, sourceNode, distances);
   
   // Check reachability
   bool reachable = isReachableFrom(fromBB, toBB, &DT, &LI, iterCount);

**Building a Call Graph**:

.. code-block:: cpp

   #include "IR/ICFG/CallGraph.h"
   
   // Build call graph from module
   LTCallGraph callGraph(module);
   
   // Iterate over functions
   for (auto it = callGraph.begin(); it != callGraph.end(); ++it) {
       const llvm::Function* func = it->first;
       LTCallGraphNode* node = it->second.get();
       
       // Iterate over callees
       for (auto& callRecord : *node) {
           llvm::Value* callSite = callRecord.first;
           LTCallGraphNode* callee = callRecord.second;
       }
   }

Integration
===========

The ICFG is used as the foundation for many analyses in Lotus:

* **IFDS/IDE**: The IFDS/IDE dataflow framework uses the ICFG to traverse
  interprocedural paths in the exploded super-graph. Call and return edges
  enable context-sensitive flow functions.

* **WPDS**: Weighted Pushdown Systems use the ICFG to model interprocedural
  control flow with weights attached to edges for value propagation.

* **PDG**: The Program Dependence Graph is built on top of the ICFG, adding
  data dependence edges to the control flow structure.

* **Reachability Analyses**: Various reachability and path-sensitive analyses
  use the ICFG to explore interprocedural execution paths.

* **CFL-Reachability**: Context-free language reachability analyses operate
  over the ICFG structure to handle matching parentheses (call/return pairs).

The ICFG's design allows analyses to seamlessly traverse both intraprocedural
and interprocedural control flow without needing to manually handle function
boundaries, making it an essential component of Lotus's analysis infrastructure.

