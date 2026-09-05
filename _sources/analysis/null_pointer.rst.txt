Null Pointer Analysis
=====================

A family of analyses for detecting possible null dereferences and tracking
null-flow through the program.

**Headers**: ``include/Analysis/NullPointer``

**Implementation**: ``lib/Analysis/NullPointer``

**Main components**:

- **AliasAnalysisAdapter** – Integrates with alias analyses for null checking
- **ContextSensitiveLocalNullCheckAnalysis** – Local, context-sensitive analysis
- **ContextSensitiveNullCheckAnalysis** – Interprocedural, context-sensitive analysis
- **ContextSensitiveNullFlowAnalysis** – Null value flow tracking
- **LocalNullCheckAnalysis** – Intraprocedural null checking
- **NullCheckAnalysis** – General null pointer analysis front-end
- **NullEquivalenceAnalysis** – Null equivalence class computation
- **NullFlowAnalysis** – Intraprocedural null propagation analysis

**Typical use cases**:

- Prove that pointer dereferences are safe (non-null)
- Find potential null dereference bugs
- Provide nullness information to optimization and verification passes

**Basic usage (C\+\+)**:

.. code-block:: cpp

   #include <Analysis/NullPointer/NullCheckAnalysis.h>

   // NullCheckAnalysis is an LLVM ModulePass. Retrieve it from a pass.
   struct MyPass : public llvm::ModulePass {
     static char ID;
     MyPass() : ModulePass(ID) {}

     bool runOnModule(llvm::Module &M) override {
       auto &NPA = getAnalysis<NullCheckAnalysis>();

       for (auto &F : M) {
         for (auto &BB : F) {
           for (auto &I : BB) {
             llvm::Value *Ptr = /* pointer operand of I */ nullptr;
             if (Ptr && NPA.mayNull(Ptr, &I)) {
               // Ptr may be null at instruction I
             }
           }
         }
       }
       return false;
     }

     void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
       AU.addRequired<NullCheckAnalysis>();
       AU.setPreservesAll();
     }
   };


