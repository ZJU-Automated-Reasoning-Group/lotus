Developer Guide
===============

This guide explains how to extend Lotus with custom analyses, checkers, and tools.

Development Environment Setup
------------------------------

Prerequisites
~~~~~~~~~~~~~

- LLVM 14.0.0 development libraries
- Z3 4.11 with headers
- CMake 3.10+
- C++17 compatible compiler (GCC 7+, Clang 5+)
- Git for version control

Building from Source
~~~~~~~~~~~~~~~~~~~~~

.. code-block:: bash

   git clone https://github.com/ZJU-PL/lotus
   cd lotus
   mkdir build && cd build
   cmake ../ -DCMAKE_BUILD_TYPE=Debug
   make -j$(nproc)

Debug build enables assertions and debugging symbols.

IDE Configuration
~~~~~~~~~~~~~~~~~

**VSCode**: Create ``.vscode/c_cpp_properties.json``:

.. code-block:: json

   {
       "configurations": [{
           "name": "Linux",
           "includePath": [
               "${workspaceFolder}/include",
               "${workspaceFolder}/build/include",
               "/path/to/llvm/include",
               "/usr/include/z3"
           ],
           "compileCommands": "${workspaceFolder}/build/compile_commands.json"
       }]
   }

**CLion**: Open the project and point to ``build/compile_commands.json``.

Code Organization
-----------------

Directory Structure
~~~~~~~~~~~~~~~~~~~

.. code-block:: text

   lotus/
   ├── include/        # Public headers
   │   ├── Alias/      # Alias analysis interfaces
   │   ├── Analysis/   # Analysis utilities
   │   ├── Apps/       # Application-level interfaces
   │   ├── CFL/        # CFL reachability
   │   ├── Dataflow/   # Data flow frameworks
   │   ├── IR/         # Intermediate representations
   │   ├── Solvers/    # Constraint solvers
   │   ├── SymbolicExecution/ # Symbolic execution
   │   ├── Transform/  # LLVM transformations
   │   └── Utils/      # Utilities
   ├── lib/            # Implementation files
   │   ├── Alias/      # Alias analysis implementations
   │   ├── Analysis/   # Analysis implementations
   │   ├── Apps/       # Applications (Clam, checkers)
   │   ├── CFL/        # CFL implementations
   │   ├── Dataflow/   # Data flow implementations
   │   ├── IR/         # IR implementations
   │   ├── Solvers/    # Solver implementations
   │   ├── SymbolicExecution/ # Symbolic execution
   │   ├── Transform/  # Transformation passes
   │   └── Utils/      # Utility implementations
   ├── tools/          # Command-line tools
   │   ├── alias/      # Alias analysis tools
   │   ├── checker/    # Bug checking tools
   │   ├── cfl/        # CFL tools
   │   └── verifier/   # Verification tools
   ├── tests/          # Test cases
   ├── benchmarks/     # Benchmark programs
   └── docs/           # Documentation

Coding Standards
~~~~~~~~~~~~~~~~

**Naming Conventions**:

- Classes: ``CamelCase`` (e.g., ``PointerAnalysis``)
- Functions: ``camelCase`` (e.g., ``getPointsToSet``)
- Variables: ``snake_case`` (e.g., ``points_to_set``)
- Constants: ``UPPER_CASE`` (e.g., ``MAX_ITERATIONS``)
- Member variables: prefix with ``m_`` or ``_``

**Code Style**:

- 2-space indentation
- 100 character line limit
- Use LLVM coding standards where applicable
- Add Doxygen comments for public APIs

Adding a New Alias Analysis
----------------------------

Step 1: Create Directory Structure
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: bash

   cd lotus
   mkdir -p include/Alias/MyAA
   mkdir -p lib/Alias/MyAA

Step 2: Define Header
~~~~~~~~~~~~~~~~~~~~~~

Create ``include/Alias/MyAA/MyAliasAnalysis.h``:

.. code-block:: cpp

   #pragma once
   
   #include "llvm/Pass.h"
   #include "llvm/IR/Module.h"
   #include "llvm/Analysis/AliasAnalysis.h"
   #include <set>
   #include <map>
   
   namespace myaa {
   
   /**
    * @brief My custom alias analysis
    * 
    * This analysis computes pointer alias information using [describe algorithm].
    */
   class MyAliasAnalysis : public llvm::ModulePass, 
                          public llvm::AliasAnalysis {
   public:
       static char ID;
       
       MyAliasAnalysis() : ModulePass(ID) {}
       
       /// Main analysis entry point
       bool runOnModule(llvm::Module &M) override;
       
       /// Query if two values may alias
       llvm::AliasResult alias(const llvm::Value *V1, 
                              const llvm::Value *V2) override;
       
       /// Get analysis usage
       void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
       
       /// Get pass name
       llvm::StringRef getPassName() const override {
           return "My Alias Analysis";
       }
       
   private:
       /// Points-to sets: pointer -> objects it may point to
       std::map<const llvm::Value*, std::set<const llvm::Value*>> points_to_;
       
       /// Compute points-to sets
       void computePointsTo(llvm::Module &M);
       
       /// Process a single instruction
       void processInstruction(llvm::Instruction *I);
   };
   
   } // namespace myaa

Step 3: Implement Analysis
~~~~~~~~~~~~~~~~~~~~~~~~~~~

Create ``lib/Alias/MyAA/MyAliasAnalysis.cpp``:

.. code-block:: cpp

   #include "Alias/MyAA/MyAliasAnalysis.h"
   #include "llvm/IR/Instructions.h"
   #include "llvm/Support/raw_ostream.h"
   
   using namespace llvm;
   using namespace myaa;
   
   char MyAliasAnalysis::ID = 0;
   
   bool MyAliasAnalysis::runOnModule(Module &M) {
       // Initialize points-to sets
       points_to_.clear();
       
       // Compute points-to information
       computePointsTo(M);
       
       return false; // Did not modify the module
   }
   
   void MyAliasAnalysis::computePointsTo(Module &M) {
       for (auto &F : M) {
           if (F.isDeclaration()) continue;
           
           for (auto &BB : F) {
               for (auto &I : BB) {
                   processInstruction(&I);
               }
           }
       }
   }
   
   void MyAliasAnalysis::processInstruction(Instruction *I) {
       // Handle different instruction types
       if (auto *alloca = dyn_cast<AllocaInst>(I)) {
           // Alloca creates a new object
           points_to_[alloca].insert(alloca);
           
       } else if (auto *load = dyn_cast<LoadInst>(I)) {
           // Load: propagate points-to from pointer
           Value *ptr = load->getPointerOperand();
           if (points_to_.count(ptr)) {
               for (auto *obj : points_to_[ptr]) {
                   // load result points to objects pointed by ptr
                   points_to_[load].insert(obj);
               }
           }
           
       } else if (auto *store = dyn_cast<StoreInst>(I)) {
           // Store: update points-to at target location
           Value *ptr = store->getPointerOperand();
           Value *val = store->getValueOperand();
           // Handle store logic...
           
       } else if (auto *gep = dyn_cast<GetElementPtrInst>(I)) {
           // GEP: field access
           Value *base = gep->getPointerOperand();
           if (points_to_.count(base)) {
               points_to_[gep] = points_to_[base];
           }
       }
       // Add more cases as needed...
   }
   
   AliasResult MyAliasAnalysis::alias(const Value *V1, const Value *V2) {
       // Check if points-to sets intersect
       if (!points_to_.count(V1) || !points_to_.count(V2)) {
           return MayAlias; // Conservative
       }
       
       const auto &pts1 = points_to_[V1];
       const auto &pts2 = points_to_[V2];
       
       // Check for intersection
       bool hasIntersection = false;
       for (auto *obj : pts1) {
           if (pts2.count(obj)) {
               hasIntersection = true;
               break;
           }
       }
       
       if (!hasIntersection) {
           return NoAlias;
       } else if (pts1 == pts2) {
           return MustAlias;
       } else {
           return MayAlias;
       }
   }
   
   void MyAliasAnalysis::getAnalysisUsage(AnalysisUsage &AU) const {
       AU.setPreservesAll();
   }
   
   // Register pass
   static RegisterPass<MyAliasAnalysis> X("my-aa", "My Alias Analysis");

Step 4: Add CMakeLists.txt
~~~~~~~~~~~~~~~~~~~~~~~~~~~

Create ``lib/Alias/MyAA/CMakeLists.txt``:

.. code-block:: cmake

   set(SOURCES
       MyAliasAnalysis.cpp
   )
   
   add_library(LotusMyAA STATIC ${SOURCES})
   
   target_include_directories(LotusMyAA PUBLIC
       ${CMAKE_SOURCE_DIR}/include
       ${LLVM_INCLUDE_DIRS}
   )
   
   target_link_libraries(LotusMyAA
       LLVMCore
       LLVMSupport
       LLVMAnalysis
   )

Update ``lib/Alias/CMakeLists.txt``:

.. code-block:: cmake

   add_subdirectory(MyAA)

Step 5: Create Command-Line Tool
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Create ``tools/alias/my-aa.cpp``:

.. code-block:: cpp

   #include "llvm/IR/LLVMContext.h"
   #include "llvm/IR/Module.h"
   #include "llvm/IRReader/IRReader.h"
   #include "llvm/Support/SourceMgr.h"
   #include "llvm/IR/LegacyPassManager.h"
   #include "llvm/Support/CommandLine.h"
   #include "Alias/MyAA/MyAliasAnalysis.h"
   
   using namespace llvm;
   
   cl::opt<std::string> InputFilename(cl::Positional, 
                                     cl::desc("<input bitcode>"), 
                                     cl::Required);
   
   cl::opt<bool> Verbose("verbose", 
                        cl::desc("Print detailed output"),
                        cl::init(false));
   
   int main(int argc, char **argv) {
       cl::ParseCommandLineOptions(argc, argv, "My Alias Analysis Tool\n");
       
       LLVMContext context;
       SMDiagnostic error;
       
       // Load module
       std::unique_ptr<Module> module = parseIRFile(InputFilename, error, context);
       if (!module) {
           error.print(argv[0], errs());
           return 1;
       }
       
       // Run analysis
       legacy::PassManager PM;
       PM.add(new myaa::MyAliasAnalysis());
       PM.run(*module);
       
       errs() << "Analysis completed successfully\n";
       return 0;
   }

Update ``tools/alias/CMakeLists.txt``:

.. code-block:: cmake

   add_executable(my-aa my-aa.cpp)
   
   target_link_libraries(my-aa
       LotusMyAA
       ${LLVM_LIBS}
   )
   
   install(TARGETS my-aa DESTINATION bin)

Step 6: Test Your Analysis
~~~~~~~~~~~~~~~~~~~~~~~~~~~

Create ``tests/alias/test_my_aa.cpp``:

.. code-block:: cpp

   int main() {
       int x = 10;
       int y = 20;
       int *p = &x;
       int *q = &y;
       // p and q should not alias
       return 0;
   }

Test:

.. code-block:: bash

   clang -emit-llvm -c tests/alias/test_my_aa.cpp -o test.bc
   ./build/bin/my-aa test.bc

Adding a New Bug Checker
-------------------------

Step 1: Define Checker Interface
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Create ``include/Checker/MyChecker.h``:

.. code-block:: cpp

   #pragma once
   
   #include "llvm/Pass.h"
   #include "llvm/IR/Module.h"
   #include "Checker/Report/BugReportMgr.h"
   #include <vector>
   
   namespace lotus {
   
   /**
    * @brief Checker for [describe bug type]
    */
   class MyChecker : public llvm::ModulePass {
   public:
       static char ID;
       
       MyChecker() : ModulePass(ID) {}
       
       bool runOnModule(llvm::Module &M) override;
       
       llvm::StringRef getPassName() const override {
           return "My Bug Checker";
       }
       
   private:
       /// Detected bugs
       std::vector<BugReport> bugs_;
       
       /// Check a single function
       void checkFunction(llvm::Function &F);
       
       /// Report a bug
       void reportBug(llvm::Instruction *I, const std::string &description);
   };
   
   } // namespace lotus

Step 2: Implement Checker
~~~~~~~~~~~~~~~~~~~~~~~~~~

Create ``lib/Checker/MyChecker.cpp``:

.. code-block:: cpp

   #include "Checker/MyChecker.h"
   #include "llvm/IR/Instructions.h"
   #include "llvm/Support/raw_ostream.h"
   
   using namespace llvm;
   using namespace lotus;
   
   char MyChecker::ID = 0;
   
   bool MyChecker::runOnModule(Module &M) {
       bugs_.clear();
       
       for (auto &F : M) {
           if (F.isDeclaration()) continue;
           checkFunction(F);
       }
       
       // Report all bugs
       errs() << "Found " << bugs_.size() << " bugs\n";
       for (auto &bug : bugs_) {
           errs() << "Bug at " << bug.location << ": " 
                  << bug.description << "\n";
       }
       
       return false;
   }
   
   void MyChecker::checkFunction(Function &F) {
       for (auto &BB : F) {
           for (auto &I : BB) {
               // Example: check for specific bug patterns
               if (auto *call = dyn_cast<CallInst>(&I)) {
                   Function *callee = call->getCalledFunction();
                   if (callee && callee->getName() == "dangerous_function") {
                       reportBug(call, "Call to dangerous function");
                   }
               }
           }
       }
   }
   
   void MyChecker::reportBug(Instruction *I, const std::string &description) {
       BugReport bug;
       bug.type = "MyBugType";
       bug.instruction = I;
       bug.description = description;
       
       // Get source location if available
       if (auto *DL = I->getDebugLoc().get()) {
           bug.file = DL->getFilename().str();
           bug.line = DL->getLine();
       }
       
       bugs_.push_back(bug);
   }
   
   static RegisterPass<MyChecker> X("my-checker", "My Bug Checker");

Step 3: Integrate with BugReportMgr
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

For SARIF/JSON output support:

.. code-block:: cpp

   #include "Checker/Report/BugReportMgr.h"
   
   bool MyChecker::runOnModule(Module &M) {
       // ... run checks ...
       
       // Report to centralized manager
       BugReportMgr &mgr = BugReportMgr::getInstance();
       for (auto &bug : bugs_) {
           mgr.addBug("MyChecker", bug);
       }
       
       return false;
   }

Adding a New Abstract Domain
-----------------------------

For CLAM integration, create a new CRAB domain:

Step 1: Define Domain Interface
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Create ``third-party/verification/clam/src/crab/domains/my_domain.hpp``:

.. code-block:: cpp

   #pragma once
   
   #include "crab/domains/abstract_domain.hpp"
   #include <map>
   
   namespace crab {
   namespace domains {
   
   template<typename Number, typename VariableName>
   class my_domain : public abstract_domain<Number, VariableName> {
   public:
       using variable_t = VariableName;
       using number_t = Number;
       
       // Required lattice operations
       my_domain() : is_bottom_(false) {}
       
       static my_domain top() {
           return my_domain(false);
       }
       
       static my_domain bottom() {
           my_domain d;
           d.is_bottom_ = true;
           return d;
       }
       
       bool is_bottom() const { return is_bottom_; }
       bool is_top() const { return !is_bottom_ && state_.empty(); }
       
       // Join (least upper bound)
       void operator|=(const my_domain &other) {
           if (is_bottom()) {
               *this = other;
           } else if (other.is_bottom()) {
               return;
           } else {
               // Implement join logic
           }
       }
       
       // Meet (greatest lower bound)
       void operator&=(const my_domain &other) {
           // Implement meet logic
       }
       
       // Widening
       my_domain operator||(const my_domain &other) {
           // Implement widening for convergence
           return *this;
       }
       
       // Transfer functions
       void assign(variable_t x, Number n) {
           // x := n
           state_[x] = n;
       }
       
       void assign(variable_t x, variable_t y) {
           // x := y
           if (state_.count(y)) {
               state_[x] = state_[y];
           }
       }
       
       void apply(operation_t op, variable_t x, variable_t y, variable_t z) {
           // x := y op z
           // Implement operation semantics
       }
       
       void operator+=(constraint_t cst) {
           // Add constraint
       }
       
       // Queries
       bool entails(constraint_t cst) const {
           // Check if domain entails constraint
           return false;
       }
       
       void write(std::ostream &o) const {
           if (is_bottom()) {
               o << "_|_";
           } else if (is_top()) {
               o << "⊤";
           } else {
               o << "{";
               for (auto &[var, val] : state_) {
                   o << var << "=" << val << " ";
               }
               o << "}";
           }
       }
       
   private:
       bool is_bottom_;
       std::map<variable_t, number_t> state_;
   };
   
   } // namespace domains
   } // namespace crab

Step 2: Register Domain
~~~~~~~~~~~~~~~~~~~~~~~~

Update ``third-party/verification/clam/src/ClamDomainRegistry.cc``:

.. code-block:: cpp

   #include "crab/domains/my_domain.hpp"
   
   // In domain factory
   if (dom_name == "mydomain") {
       return std::make_unique<my_domain<number_t, varname_t>>();
   }

Step 3: Add Command-Line Option
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Update ``tools/verifier/clam/clam.cc``:

.. code-block:: cpp

   // Add to domain options
   cl::opt<std::string> CrabDomain(
       "crab-dom",
       cl::desc("Abstract domain"),
       cl::values(
           clEnumValN(INTERVALS, "int", "Classical interval domain"),
           clEnumValN(MY_DOMAIN, "mydomain", "My custom domain"),
           // ...
       ),
       cl::init(INTERVALS));

Adding a Data Flow Analysis
----------------------------

Using the IFDS/IDE framework:

Step 1: Define Analysis Problem
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Create ``lib/Dataflow/MyDataFlow/MyAnalysis.h``:

.. code-block:: cpp

   #pragma once
   
   #include "Dataflow/IFDS/IFDSProblem.h"
   
   namespace lotus {
   
   /**
    * @brief My custom data flow analysis
    */
   class MyAnalysis : public IFDSProblem<llvm::Value*, llvm::Function*> {
   public:
       using Fact = llvm::Value*;
       using Context = llvm::Function*;
       
       MyAnalysis(llvm::Module &M) : IFDSProblem(M) {}
       
       // Initial facts (sources)
       std::set<Fact> initialSeeds() override;
       
       // Normal flow within basic blocks
       FlowFunctionPtr getNormalFlowFunction(
           llvm::Instruction *curr,
           llvm::Instruction *succ) override;
       
       // Call flow (caller -> callee)
       FlowFunctionPtr getCallFlowFunction(
           llvm::Instruction *callSite,
           llvm::Function *destFun) override;
       
       // Return flow (callee -> caller)
       FlowFunctionPtr getReturnFlowFunction(
           llvm::Instruction *callSite,
           llvm::Function *calleeFun,
           llvm::Instruction *exitInst) override;
       
       // Call-to-return flow (bypass call)
       FlowFunctionPtr getCallToReturnFlowFunction(
           llvm::Instruction *callSite,
           llvm::Instruction *returnSite) override;
   };
   
   } // namespace lotus

Step 2: Implement Flow Functions
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Create ``lib/Dataflow/MyDataFlow/MyAnalysis.cpp``:

.. code-block:: cpp

   #include "Dataflow/MyDataFlow/MyAnalysis.h"
   #include "Dataflow/IFDS/FlowFunctions.h"
   
   using namespace llvm;
   using namespace lotus;
   
   std::set<Value*> MyAnalysis::initialSeeds() {
       std::set<Value*> seeds;
       
       // Add initial facts (e.g., function parameters, globals)
       for (auto &F : module) {
           if (F.getName() == "source_function") {
               // Return value is a source
               seeds.insert(&F);
           }
       }
       
       return seeds;
   }
   
   FlowFunctionPtr MyAnalysis::getNormalFlowFunction(
       Instruction *curr, Instruction *succ) {
       
       // Identity flow by default
       if (!affectsAnalysis(curr)) {
           return std::make_shared<Identity<Fact>>();
       }
       
       // Generate new facts
       if (auto *alloca = dyn_cast<AllocaInst>(curr)) {
           return std::make_shared<Gen<Fact>>(alloca);
       }
       
       // Kill facts
       if (isSanitizer(curr)) {
           return std::make_shared<KillAll<Fact>>();
       }
       
       // Conditional generation
       return std::make_shared<ConditionalGen<Fact>>(curr, 
           [](Fact f) { return shouldPropagate(f); });
   }
   
   FlowFunctionPtr MyAnalysis::getCallFlowFunction(
       Instruction *callSite, Function *destFun) {
       
       // Map actual parameters to formals
       auto *call = cast<CallInst>(callSite);
       
       return std::make_shared<MapParametersFlowFunction>(
           call, destFun);
   }
   
   FlowFunctionPtr MyAnalysis::getReturnFlowFunction(
       Instruction *callSite, Function *calleeFun, 
       Instruction *exitInst) {
       
       // Map return value to call site
       if (auto *ret = dyn_cast<ReturnInst>(exitInst)) {
           return std::make_shared<MapReturnFlowFunction>(
               callSite, ret);
       }
       
       return std::make_shared<Identity<Fact>>();
   }

Step 3: Create Solver and Run
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: cpp

   #include "Dataflow/IFDS/IFDSSolver.h"
   #include "Dataflow/MyDataFlow/MyAnalysis.h"
   
   // Create problem
   MyAnalysis problem(module);
   
   // Create solver
   IFDSSolver<Value*, Function*> solver(problem);
   
   // Solve
   solver.solve();
   
   // Query results
   for (auto &F : module) {
       for (auto &BB : F) {
           for (auto &I : BB) {
               auto facts = solver.ifdsResultsAt(&I);
               // Process facts...
           }
       }
   }

Testing and Debugging
---------------------

Writing Unit Tests
~~~~~~~~~~~~~~~~~~

Create ``tests/unit/test_my_analysis.cpp``:

.. code-block:: cpp

   #include "gtest/gtest.h"
   #include "llvm/IR/LLVMContext.h"
   #include "llvm/IR/Module.h"
   #include "Alias/MyAA/MyAliasAnalysis.h"
   
   class MyAnalysisTest : public ::testing::Test {
   protected:
       llvm::LLVMContext context;
       std::unique_ptr<llvm::Module> module;
       
       void SetUp() override {
           module = std::make_unique<llvm::Module>("test", context);
       }
   };
   
   TEST_F(MyAnalysisTest, BasicAliasQuery) {
       // Create test IR
       auto *funcType = llvm::FunctionType::get(
           llvm::Type::getVoidTy(context), false);
       auto *func = llvm::Function::Create(
           funcType, llvm::Function::ExternalLinkage, "test", module.get());
       auto *bb = llvm::BasicBlock::Create(context, "entry", func);
       
       // Add instructions...
       
       // Run analysis
       myaa::MyAliasAnalysis aa;
       aa.runOnModule(*module);
       
       // Check results
       auto result = aa.alias(val1, val2);
       EXPECT_EQ(result, llvm::NoAlias);
   }

Run tests:

.. code-block:: bash

   cd build
   make test

Debugging Tips
~~~~~~~~~~~~~~

**1. Enable debug output**:

.. code-block:: cpp

   #define DEBUG_TYPE "my-analysis"
   #include "llvm/Support/Debug.h"
   
   LLVM_DEBUG(dbgs() << "Processing instruction: " << *I << "\n");

Run with debug output:

.. code-block:: bash

   ./my-tool -debug-only=my-analysis input.bc

**2. Dump intermediate results**:

.. code-block:: cpp

   errs() << "Points-to set for " << *ptr << ":\n";
   for (auto *obj : pts) {
       errs() << "  - " << *obj << "\n";
   }

**3. Use LLVM's verification passes**:

.. code-block:: cpp

   PM.add(createVerifierPass()); // Check IR validity

**4. GDB debugging**:

.. code-block:: bash

   gdb --args ./my-tool input.bc
   (gdb) break MyAliasAnalysis::runOnModule
   (gdb) run

Performance Optimization
------------------------

Profiling
~~~~~~~~~

Use LLVM's time-passes:

.. code-block:: bash

   ./my-tool -time-passes input.bc

Optimization Strategies
~~~~~~~~~~~~~~~~~~~~~~~

1. **Use Sparse Data Structures**: BitVector for points-to sets
2. **Cache Results**: Memoize expensive computations
3. **Early Termination**: Detect fixpoints early
4. **SCC Collapsing**: Merge strongly connected components
5. **Incremental Analysis**: Reuse results across runs

Example caching:

.. code-block:: cpp

   class CachedAnalysis {
       std::map<Value*, PointsToSet> cache_;
       
   public:
       const PointsToSet& getPointsTo(Value *v) {
           auto it = cache_.find(v);
           if (it != cache_.end()) {
               return it->second; // Return cached result
           }
           
           // Compute and cache
           PointsToSet pts = computePointsTo(v);
           cache_[v] = pts;
           return cache_[v];
       }
   };

Contributing to Lotus
---------------------

Contribution Workflow
~~~~~~~~~~~~~~~~~~~~~

1. **Fork** the repository on GitHub
2. **Create** a feature branch: ``git checkout -b my-feature``
3. **Implement** your changes with tests
4. **Document** your changes in relevant docs
5. **Test** thoroughly: ``make test``
6. **Commit** with clear messages: ``git commit -m "Add my feature"``
7. **Push**: ``git push origin my-feature``
8. **Create** a pull request on GitHub

Code Review Process
~~~~~~~~~~~~~~~~~~~

- All code must pass CI tests
- At least one approval from maintainers required
- Follow coding standards
- Add documentation and tests
- Keep PRs focused and manageable

Documentation Updates
~~~~~~~~~~~~~~~~~~~~~

When adding new features:

1. Update relevant ``.rst`` files in ``docs/source/``
2. Add API documentation in ``api_reference.rst``
3. Add tutorial in ``tutorials.rst`` if applicable
4. Add new tool documentation in ``docs/source/tools/``

Build documentation:

.. code-block:: bash

   cd docs
   make html
   # View at docs/build/html/index.html

Common Pitfalls
---------------

Memory Management
~~~~~~~~~~~~~~~~~

**Problem**: LLVM uses custom allocators and memory management.

**Solution**: Use LLVM's memory management properly:

.. code-block:: cpp

   // Don't do this:
   BasicBlock *bb = new BasicBlock(...);
   
   // Do this:
   BasicBlock *bb = BasicBlock::Create(context, "name", func);

**Problem**: Dangling pointers after IR modification.

**Solution**: Use ``Value::replaceAllUsesWith()`` and proper IR building.

Pass Dependencies
~~~~~~~~~~~~~~~~~

**Problem**: Analysis depends on another pass but doesn't declare it.

**Solution**: Declare dependencies in ``getAnalysisUsage()``:

.. code-block:: cpp

   void getAnalysisUsage(AnalysisUsage &AU) const override {
       AU.addRequired<DominatorTreeWrapperPass>();
       AU.addRequired<DyckAliasAnalysis>();
       AU.setPreservesAll();
   }

Fixpoint Convergence
~~~~~~~~~~~~~~~~~~~~

**Problem**: Analysis doesn't converge (infinite loop).

**Solution**: Implement proper widening and set iteration limit:

.. code-block:: cpp

   const int MAX_ITERATIONS = 100;
   int iter = 0;
   while (changed && iter < MAX_ITERATIONS) {
       // Analysis logic...
       iter++;
   }
   if (iter >= MAX_ITERATIONS) {
       errs() << "Warning: Analysis did not converge\n";
   }

Resources
---------

- **LLVM Documentation**: https://llvm.org/docs/
- **LLVM Programmer's Manual**: https://llvm.org/docs/ProgrammersManual.html
- **Writing LLVM Pass**: https://llvm.org/docs/WritingAnLLVMPass.html
- **Lotus Repository**: https://github.com/ZJU-PL/lotus
- **Issue Tracker**: https://github.com/ZJU-PL/lotus/issues

Community
---------

- **GitHub Discussions**: For questions and discussions
- **Issue Tracker**: For bug reports and feature requests
- **Pull Requests**: For code contributions

See Also
--------

- :doc:`../user_guide/architecture` - Framework architecture
- :doc:`api_reference` - API documentation
- :doc:`../user_guide/tutorials` - Usage examples
