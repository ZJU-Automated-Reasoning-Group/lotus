#include "Verification/Transform/MarkVolatile.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"

using namespace llvm;

namespace lotus {
namespace verification {
namespace transform {

char MarkVolatilePass::ID = 0;

bool MarkVolatilePass::runOnFunction(Function &F) {
  if (F.isDeclaration())
    return false;

  bool modified = false;
  LLVMContext &ctx = F.getParent()->getContext();

  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
    Instruction *ins = &*I;
    if (CallInst *CI = dyn_cast<CallInst>(ins)) {
      if (CI->isInlineAsm())
        continue;

      const Value *val = CI->getCalledOperand()->stripPointerCasts();
      const Function *callee = dyn_cast<Function>(val);
      if (!callee || callee->isIntrinsic())
        continue;

      if (!callee->hasName())
        continue;

      StringRef name = callee->getName();

      if (!name.startswith("__INSTR_mark_"))
        continue;

      // Found a marked instruction, make it volatile
      // if it is store or load
      auto nextIt = I;
      ++nextIt;
      if (nextIt == E)
        continue;

      if (StoreInst *SI = dyn_cast<StoreInst>(&*nextIt)) {
        SI->setVolatile(true);
        modified = true;
      } else if (LoadInst *LI = dyn_cast<LoadInst>(&*nextIt)) {
        LI->setVolatile(true);
        modified = true;
      } else if (MemIntrinsic *MI = dyn_cast<MemIntrinsic>(&*nextIt)) {
        MI->setVolatile(ConstantInt::getTrue(ctx));
        modified = true;
      }
    }
  }
  return modified;
}

} // namespace transform
} // namespace verification
} // namespace lotus

static llvm::RegisterPass<lotus::verification::transform::MarkVolatilePass>
    X("mark-volatile", "Make marked instructions as volatile");

namespace lotus {
namespace verification {
namespace transform {

llvm::Pass *createMarkVolatilePass() { return new MarkVolatilePass(); }

} // namespace transform
} // namespace verification
} // namespace lotus
