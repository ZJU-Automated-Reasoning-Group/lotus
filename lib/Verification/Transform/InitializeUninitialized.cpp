#include "Verification/Transform/InitializeUninitialized.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"

using namespace llvm;

namespace lotus {
namespace verification {
namespace transform {

char InitializeUninitializedPass::ID = 0;

bool InitializeUninitializedPass::runOnModule(Module &M) {
  bool changed = false;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    BasicBlock &Entry = F.getEntryBlock();
    for (Instruction &I : Entry) {
      auto *AI = dyn_cast<AllocaInst>(&I);
      if (!AI)
        continue;

      Type *Ty = AI->getAllocatedType();
      if (Ty == nullptr || !Ty->isFirstClassType() || Ty->isAggregateType())
        continue;

      Instruction *InsertPt = AI->getNextNode();
      if (InsertPt == nullptr)
        continue;
      IRBuilder<> B(InsertPt);

      std::string Name = "verifier.nondet.init.";
      if (Ty->isIntegerTy())
        Name += "i" + std::to_string(Ty->getIntegerBitWidth());
      else if (Ty->isFloatingPointTy())
        Name += "fp";
      else if (Ty->isPointerTy())
        Name += "ptr";
      else
        Name += "val";

      FunctionCallee ND =
          M.getOrInsertFunction(Name, FunctionType::get(Ty, false));
      Value *V = B.CreateCall(ND);
      B.CreateStore(V, AI);
      changed = true;
    }
  }
  return changed;
}

} // namespace transform
} // namespace verification
} // namespace lotus

static llvm::RegisterPass<
    lotus::verification::transform::InitializeUninitializedPass>
    X("initialize-uninitialized",
      "Initialize stack allocas with nondeterministic values");

namespace lotus {
namespace verification {
namespace transform {

llvm::Pass *createInitializeUninitializedPass() {
  return new InitializeUninitializedPass();
}

} // namespace transform
} // namespace verification
} // namespace lotus
