API Reference
=============

This document provides API reference for programmatically using Lotus components.

Alias Analysis APIs
-------------------

DyckAA API
~~~~~~~~~~

Include headers:

.. code-block:: cpp

   #include "Alias/UnificationBased/DyckAA/DyckAliasAnalysis.h"

Basic usage as LLVM pass:

.. code-block:: cpp

   #include "llvm/IR/LLVMContext.h"
   #include "llvm/IR/Module.h"
   #include "llvm/IRReader/IRReader.h"
   #include "llvm/Support/SourceMgr.h"
   #include "llvm/IR/LegacyPassManager.h"
   #include "Alias/UnificationBased/DyckAA/DyckAliasAnalysis.h"
   
   using namespace llvm;
   
   int main(int argc, char **argv) {
       LLVMContext context;
       SMDiagnostic error;
       
       // Load module
       std::unique_ptr<Module> module = parseIRFile(argv[1], error, context);
       if (!module) {
           error.print(argv[0], errs());
           return 1;
       }
       
       // Run DyckAA
       legacy::PassManager PM;
       PM.add(new DyckAliasAnalysis());
       PM.run(*module);
       
       return 0;
   }

Query alias information:

.. code-block:: cpp

   // Get alias analysis result
   DyckAliasAnalysis *DAA = new DyckAliasAnalysis();
   
   // Query if two values may alias
   AliasResult result = DAA->alias(val1, val2);
   
   switch (result) {
       case NoAlias:
           errs() << "Definitely do not alias\n";
           break;
       case MayAlias:
           errs() << "May alias\n";
           break;
       case MustAlias:
           errs() << "Must alias\n";
           break;
       case PartialAlias:
           errs() << "Partially alias\n";
           break;
   }

Access Dyck graph:

.. code-block:: cpp

   #include "Alias/UnificationBased/DyckAA/DyckGraph.h"
   
   DyckGraph *graph = DAA->getDyckGraph();
   
   // Get representative for a value
   DyckGraphNode *node = graph->retrieveDyckVertex(value);
   DyckGraphNode *rep = node->getRep();
   
   // Iterate over alias set
   for (auto *aliasNode : rep->getAliasSet()) {
       Value *aliasValue = aliasNode->getValue();
       errs() << "Alias: " << *aliasValue << "\n";
   }

AserPTA API
~~~~~~~~~~~

Include headers:

.. code-block:: cpp

   #include "Alias/InclusionBased/AserPTA/PointerAnalysis/PointerAnalysis.h"
   #include "Alias/InclusionBased/AserPTA/PointerAnalysis/Context/Context.h"
   #include "Alias/InclusionBased/AserPTA/PointerAnalysis/Solver/Solver.h"

Configure and run analysis:

.. code-block:: cpp

   #include "Alias/InclusionBased/AserPTA/PointerAnalysis/Program/Program.h"
   #include "Alias/InclusionBased/AserPTA/PointerAnalysis/PointerAnalysis.h"
   
   using namespace aser;
   
   // Build program representation
   Program program(module);
   program.build();
   
   // Create analysis with context sensitivity
   PointerAnalysis<Context::KCallSite<1>, MemModel::FieldSensitive> pta(program);
   
   // Run solver
   pta.solve();
   
   // Query points-to sets
   for (auto *val : program.getPointers()) {
       const PointsToSet &pts = pta.getPointsToSet(val);
       errs() << "Pointer: " << *val << " points to:\n";
       for (auto objID : pts) {
           errs() << "  Object " << objID << "\n";
       }
   }

Custom context sensitivity:

.. code-block:: cpp

   // Context-insensitive
   PointerAnalysis<Context::NoCtx, MemModel::FieldInsensitive> pta_ci(program);
   
   // 1-CFA (1-call-site sensitive)
   PointerAnalysis<Context::KCallSite<1>, MemModel::FieldSensitive> pta_1cfa(program);
   
   // 2-CFA
   PointerAnalysis<Context::KCallSite<2>, MemModel::FieldSensitive> pta_2cfa(program);
   
   // Origin-sensitive
   PointerAnalysis<Context::KOrigin<1>, MemModel::FieldSensitive> pta_origin(program);

LotusAA API
~~~~~~~~~~~

Include headers:

.. code-block:: cpp

   #include "Alias/InclusionBased/LotusAA/Engine/InterProceduralPass.h"

Use as LLVM pass:

.. code-block:: cpp

   legacy::PassManager PM;
   PM.add(new LotusAA());
   PM.run(*module);

Query results:

.. code-block:: cpp

   LotusAA *lotus = new LotusAA();
   // ... run analysis ...
   
   // Get points-to graph
   PointsToGraph *ptg = lotus->getPointsToGraph();
   
   // Query points-to set for a value
   std::set<MemObject*> pts = ptg->getPointsToSet(value);
   
   for (auto *obj : pts) {
       errs() << "Points to: " << obj->toString() << "\n";
   }

Intermediate Representation APIs
---------------------------------

Program Dependence Graph API
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Include headers:

.. code-block:: cpp

   #include "IR/PDG/Core/ProgramDependencyGraph.h"
   #include "IR/PDG/Core/Graph.h"

Build PDG:

.. code-block:: cpp

   using namespace pdg;
   
   // Register required passes
   legacy::PassManager PM;
   PM.add(new DataDependencyGraph());
   PM.add(new ControlDependencyGraph());
   PM.add(new ProgramDependencyGraph());
   PM.run(*module);
   
   // Get PDG instance
   ProgramGraph *pdg = &ProgramGraph::getInstance();

Traverse PDG:

.. code-block:: cpp

   // Iterate over all nodes
   for (auto *node : pdg->getNodes()) {
       errs() << "Node type: " << node->getNodeTypeStr() << "\n";
       
       // Get outgoing edges
       for (auto *edge : node->getOutEdges()) {
           Node *target = edge->getTarget();
           EdgeType type = edge->getEdgeType();
           errs() << "  -> " << target->getNodeTypeStr() 
                  << " (edge: " << edge->getEdgeTypeStr() << ")\n";
       }
   }

Query node types:

.. code-block:: cpp

   // Get function wrapper
   Function *F = /* ... */;
   FunctionWrapper *fw = pdg->getFuncWrapperMap()[F];
   
   // Get entry node
   Node *entryNode = fw->getEntryNode();
   
   // Get formal input parameters
   Tree *formalInTree = fw->getFormalInTree();
   
   // Get formal output (return values)
   Tree *formalOutTree = fw->getFormalOutTree();

Check dependencies:

.. code-block:: cpp

   // Check if there's a path from src to dst
   bool hasPath = pdg->canReach(*srcNode, *dstNode);
   
   // Check reachability excluding certain edge types
   std::set<EdgeType> exclude = {EdgeType::CONTROL_DEP};
   bool hasDataPath = pdg->canReach(*srcNode, *dstNode, exclude);

PDG Cypher Query API
~~~~~~~~~~~~~~~~~~~~~

Include headers:

.. code-block:: cpp

   #include "IR/PDG/QueryLanguage/Cypher.h"
   #include "IR/PDG/Core/ProgramDependencyGraph.h"

Parse and execute Cypher queries:

.. code-block:: cpp

   using namespace pdg;
   
   // Build PDG first
   ProgramDependencyGraph *pdgPass = /* get pass */;
   ProgramGraph &pdg = pdgPass->getPDG();
   
   // Create Cypher query executor
   CypherQueryExecutor executor(pdg);
   
   // Parse Cypher query
   CypherParser parser;
   std::string queryStr = "MATCH (n:FUNC_ENTRY) WHERE n.name = 'main' RETURN n";
   std::unique_ptr<CypherQuery> query = parser.parse(queryStr);
   
   // Execute query
   std::unique_ptr<CypherResult> result = executor.execute(*query);
   
   // Process results
   if (result->getType() == CypherResult::ResultType::NODES) {
       for (auto *node : result->getNodes()) {
           errs() << "Result node: " << *node << "\n";
       }
   }

Execute queries with error handling:

.. code-block:: cpp

   CypherParser parser;
   std::string queryStr = "MATCH (n) RETURN n";
   std::unique_ptr<CypherQuery> query = parser.parse(queryStr);
   
   if (parser.hasError()) {
       errs() << "Parse error: " << parser.getLastError().toString() << "\n";
       return;
   }
   
   CypherQueryExecutor executor(pdg);
   std::unique_ptr<CypherResult> result = executor.execute(*query);
   
   if (!executor.getLastError().empty()) {
       errs() << "Execution error: " << executor.getLastError() << "\n";
       return;
   }

Query with parameters:

.. code-block:: cpp

   CypherQueryParameters params;
   params["functionName"] = "main";
   
   std::string queryStr = "MATCH (n:FUNC_ENTRY) WHERE n.name = $functionName RETURN n";
   std::unique_ptr<CypherQuery> query = parser.parse(queryStr, params);

Data Flow Analysis APIs
-----------------------

IFDS/IDE Framework API
~~~~~~~~~~~~~~~~~~~~~~

Include headers:

.. code-block:: cpp

   #include "Dataflow/IFDS/IFDSSolver.h"
   #include "Dataflow/IFDS/FlowFunction.h"

Define flow functions:

.. code-block:: cpp

   #include "Dataflow/IFDS/IFDSProblem.h"
   
   class MyTaintAnalysis : public IFDSProblem<Value*, Function*> {
   public:
       // Define initial seeds
       std::set<Value*> initialSeeds() override {
           std::set<Value*> seeds;
           // Add taint sources
           for (auto &F : module) {
               if (F.getName() == "getInput") {
                   // Return value is tainted
                   seeds.insert(&F);
               }
           }
           return seeds;
       }
       
       // Normal flow function
       FlowFunctionPtr getNormalFlowFunction(Instruction *curr, 
                                             Instruction *succ) override {
           // Define how facts flow through instructions
           if (auto *call = dyn_cast<CallInst>(curr)) {
               if (isSanitizer(call)) {
                   // Kill taint at sanitizer
                   return std::make_shared<KillAllFlowFunction>();
               }
           }
           // Identity by default
           return std::make_shared<IdentityFlowFunction>();
       }
       
       // Call flow function
       FlowFunctionPtr getCallFlowFunction(Instruction *callSite,
                                           Function *destFun) override {
           // Map actual parameters to formal parameters
           return std::make_shared<ParameterMapFlowFunction>(callSite, destFun);
       }
       
       // Return flow function
       FlowFunctionPtr getReturnFlowFunction(Instruction *callSite,
                                             Function *calleeFun,
                                             Instruction *exitInst) override {
           // Map return value to call site
           return std::make_shared<ReturnMapFlowFunction>(callSite, exitInst);
       }
   };

Run IFDS solver:

.. code-block:: cpp

   MyTaintAnalysis problem(module);
   IFDSSolver<Value*, Function*> solver(problem);
   solver.solve();
   
   // Query results
   for (auto &F : module) {
       for (auto &BB : F) {
           for (auto &I : BB) {
               auto facts = solver.ifdsResultsAt(&I);
               if (!facts.empty()) {
                   errs() << "Tainted at: " << I << "\n";
                   for (auto *fact : facts) {
                       errs() << "  Fact: " << *fact << "\n";
                   }
               }
           }
       }
   }

Taint Analysis API
~~~~~~~~~~~~~~~~~~

Include headers:

.. code-block:: cpp

   #include "Dataflow/TaintAnalysis/TaintAnalysis.h"

Configure and run:

.. code-block:: cpp

   // Create taint analysis
   TaintAnalysis taint(module);
   
   // Configure sources
   taint.addSourceFunction("scanf");
   taint.addSourceFunction("gets");
   taint.addSourceFunction("read");
   
   // Configure sinks
   taint.addSinkFunction("system");
   taint.addSinkFunction("exec");
   taint.addSinkFunction("popen");
   
   // Run analysis
   taint.analyze();
   
   // Get results
   std::vector<TaintFlow> flows = taint.getTaintFlows();
   for (auto &flow : flows) {
       errs() << "Taint flow:\n";
       errs() << "  Source: " << *flow.source << "\n";
       errs() << "  Sink: " << *flow.sink << "\n";
       errs() << "  Path length: " << flow.pathLength << "\n";
   }

Abstract Interpretation APIs
-----------------------------

CLAM API
~~~~~~~~

Include headers:

.. code-block:: cpp

   #include "Apps/clam/Clam.hh"
   #include "Apps/clam/ClamAnalysisParams.hh"

Configure analysis:

.. code-block:: cpp

   using namespace clam;
   
   // Set analysis parameters
   ClamAnalysisParams params;
   params.dom = "zones";  // Abstract domain
   params.run_inter = true;  // Interprocedural
   params.check_nulls = true;  // Check null pointers
   params.check_bounds = true;  // Check array bounds
   
   // Create analyzer
   ClamGlobalAnalysis analyzer(module, params);
   
   // Run analysis
   analyzer.analyze();

Query invariants:

.. code-block:: cpp

   // Get invariants at a program point
   for (auto &F : module) {
       for (auto &BB : F) {
           clam_abstract_domain inv = analyzer.get_pre(&BB);
           
           // Check if a variable is in range
           if (inv.is_bottom()) {
               errs() << "Unreachable basic block\n";
           } else {
               errs() << "Invariant at " << BB.getName() << ": ";
               inv.write(errs());
               errs() << "\n";
           }
       }
   }

Check properties:

.. code-block:: cpp

   #include "crab/checkers/base_property.hpp"
   
   // Get check results
   auto checks = analyzer.get_all_checks();
   
   for (auto &check : checks) {
       switch (check.result) {
           case crab::checker::check_kind::SAFE:
               errs() << "SAFE: " << check.description << "\n";
               break;
           case crab::checker::check_kind::WARNING:
               errs() << "WARNING: " << check.description << "\n";
               break;
           case crab::checker::check_kind::ERROR:
               errs() << "ERROR: " << check.description << "\n";
               break;
       }
   }

Constraint Solving APIs
-----------------------

SMT Solver API
~~~~~~~~~~~~~~

Include headers:

.. code-block:: cpp

   #include "Solvers/SMT/Z3Solver.h"

Basic usage:

.. code-block:: cpp

   using namespace lotus::smt;
   
   // Create Z3 context and solver
   Z3Solver solver;
   
   // Create variables
   z3::expr x = solver.makeIntVar("x");
   z3::expr y = solver.makeIntVar("y");
   
   // Add constraints
   solver.addConstraint(x >= 0);
   solver.addConstraint(x <= 10);
   solver.addConstraint(y == x + 5);
   
   // Check satisfiability
   if (solver.check() == z3::sat) {
       z3::model model = solver.getModel();
       int x_val = model.eval(x).get_numeral_int();
       int y_val = model.eval(y).get_numeral_int();
       errs() << "Solution: x = " << x_val << ", y = " << y_val << "\n";
   } else {
       errs() << "Unsatisfiable\n";
   }

Build complex formulas:

.. code-block:: cpp

   // Logical operations
   z3::expr formula = (x > 0) && (y < 10) || (x == y);
   
   // Arrays
   z3::expr arr = solver.makeArrayVar("arr", solver.intSort());
   z3::expr arrSelect = z3::select(arr, x);
   solver.addConstraint(arrSelect == 42);
   
   // Quantifiers
   z3::expr i = solver.makeIntVar("i");
   z3::expr forall = z3::forall(i, z3::implies(i >= 0 && i < 10, 
                                                z3::select(arr, i) >= 0));

BDD Solver API
~~~~~~~~~~~~~~

Include headers:

.. code-block:: cpp

   #include "Solvers/BDD/BDDSolver.h"

Basic operations:

.. code-block:: cpp

   using namespace lotus::bdd;
   
   // Initialize BDD manager
   BDDManager mgr;
   
   // Create variables
   BDD x = mgr.createVar("x");
   BDD y = mgr.createVar("y");
   BDD z = mgr.createVar("z");
   
   // Boolean operations
   BDD formula = (x | y) & (~z);
   
   // Check satisfiability
   if (!formula.isZero()) {
       errs() << "Formula is satisfiable\n";
       
       // Get one satisfying assignment
       std::map<std::string, bool> assignment = formula.oneSat();
       for (auto &[var, val] : assignment) {
           errs() << var << " = " << (val ? "true" : "false") << "\n";
       }
   }

Bug Detection APIs
------------------

Kint API
~~~~~~~~

Include headers:

.. code-block:: cpp

   #include "Checker/KINT/MKintPass.h"

Use programmatically:

.. code-block:: cpp

   // Create pass
   MKintPass kintPass;
   
   // Configure checkers
   kintPass.enableChecker(CheckerType::IntOverflow);
   kintPass.enableChecker(CheckerType::ArrayOutOfBounds);
   
   // Run on module
   kintPass.runOnModule(module);
   
   // Get bug reports
   std::vector<BugReport> bugs = kintPass.getBugReports();
   
   for (auto &bug : bugs) {
       errs() << "Bug Type: " << bug.type << "\n";
       errs() << "Location: " << bug.location << "\n";
       errs() << "Description: " << bug.description << "\n";
   }

Utility APIs
------------

Call Graph API
~~~~~~~~~~~~~~

Include headers:

.. code-block:: cpp

   #include "IR/PDG/Core/PDGCallGraph.h"

Build and query call graph:

.. code-block:: cpp

   using namespace pdg;
   
   PDGCallGraph &cg = PDGCallGraph::getInstance();
   cg.build(module);
   
   // Get callers of a function
   Function *F = /* ... */;
   std::set<CallInst*> callers = cg.getCallers(F);
   
   // Get callees of a call site
   CallInst *call = /* ... */;
   std::set<Function*> callees = cg.getCallees(call);
   
   // Check if function is reachable
   bool reachable = cg.isReachable(mainFunc, targetFunc);

Memory Model API
~~~~~~~~~~~~~~~~

Include headers:

.. code-block:: cpp

   #include "Alias/InclusionBased/AserPTA/PointerAnalysis/Model/MemModels.h"

Query memory objects:

.. code-block:: cpp

   // Get object ID for a value
   ObjID obj = memModel.getObject(value);
   
   // Get field offset
   FieldOffset offset = memModel.getFieldOffset(gepInst);
   
   // Check if field-sensitive
   if (memModel.isFieldSensitive()) {
       errs() << "Using field-sensitive model\n";
   }

Configuration API
~~~~~~~~~~~~~~~~~

Include headers:

.. code-block:: cpp

   #include "Utils/Config/LotusConfig.h"

Load and use configuration:

.. code-block:: cpp

   // Load YAML configuration
   LotusConfig config;
   config.loadFromFile("config.yaml");
   
   // Query configuration values
   int timeout = config.getInt("analysis.timeout", 300);
   bool verbose = config.getBool("logging.verbose", false);
   std::string domain = config.getString("clam.domain", "zones");
   
   // Set configuration programmatically
   config.set("analysis.max_iterations", 100);
   config.set("checker.enabled", true);

Example: Complete Analysis Tool
--------------------------------

Here's a complete example of building a custom analysis tool:

.. code-block:: cpp

   #include "llvm/IR/LLVMContext.h"
   #include "llvm/IR/Module.h"
   #include "llvm/IRReader/IRReader.h"
   #include "llvm/Support/SourceMgr.h"
   #include "llvm/IR/LegacyPassManager.h"
   #include "Alias/UnificationBased/DyckAA/DyckAliasAnalysis.h"
   #include "IR/PDG/Core/ProgramDependencyGraph.h"
   #include "Dataflow/TaintAnalysis/TaintAnalysis.h"
   #include "Checker/KINT/MKintPass.h"
   
   using namespace llvm;
   
   int main(int argc, char **argv) {
       if (argc < 2) {
           errs() << "Usage: " << argv[0] << " <input.bc>\n";
           return 1;
       }
       
       // Load module
       LLVMContext context;
       SMDiagnostic error;
       std::unique_ptr<Module> module = parseIRFile(argv[1], error, context);
       
       if (!module) {
           error.print(argv[0], errs());
           return 1;
       }
       
       // Run analyses
       legacy::PassManager PM;
       
       // 1. Alias analysis
       PM.add(new DyckAliasAnalysis());
       
       // 2. Build PDG
       PM.add(new pdg::DataDependencyGraph());
       PM.add(new pdg::ControlDependencyGraph());
       PM.add(new pdg::ProgramDependencyGraph());
       
       // 3. Bug detection
       PM.add(new MKintPass());
       
       PM.run(*module);
       
       // 4. Taint analysis
       TaintAnalysis taint(*module);
       taint.addSourceFunction("scanf");
       taint.addSinkFunction("system");
       taint.analyze();
       
       // Print results
       auto flows = taint.getTaintFlows();
       errs() << "Found " << flows.size() << " taint flows\n";
       
       return 0;
   }

Compile and link:

.. code-block:: bash

   clang++ -o my_analyzer my_analyzer.cpp \
       $(llvm-config --cxxflags --ldflags --libs) \
       -L/path/to/lotus/build/lib \
       -lLotusAlias -lLotusIR -lLotusDataflow -lLotusChecker

See Also
--------

- :doc:`../user_guide/architecture` - Understanding the framework architecture
- :doc:`../user_guide/tutorials` - Practical usage examples
- :doc:`developer_guide` - Extending Lotus
