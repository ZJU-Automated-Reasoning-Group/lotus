#include "Verification/Transform/RemoveConstantExprs.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"

using namespace llvm;

namespace lotus {
namespace verification {
namespace transform {

char RemoveConstantExprsPass::ID = 0;

static inline ConstantExpr *getCEOperand(Instruction &I) {
  for (auto &op : I.operands()) {
    if (auto *CE = dyn_cast<ConstantExpr>(op)) {
      return CE;
    }
  }
  return nullptr;
}

template <typename Queue>
static void queueCEInst(Queue &queue, Instruction &I) {
  if (auto *CE = getCEOperand(I)) {
    queue.insert(std::make_pair(&I, CE));
  }
}

bool RemoveConstantExprsPass::runOnModule(Module &M) {
  bool changed = false;
  for (Function &F : M) {
    if (F.isDeclaration())
      continue;

    std::set<std::pair<Instruction *, ConstantExpr *>> instsWithCE;

    for (auto &I : instructions(F)) {
      queueCEInst(instsWithCE, I);
    }

    while (!instsWithCE.empty()) {
      auto cur = instsWithCE.begin();
      auto *I = cur->first;
      auto *CE = cur->second;
      instsWithCE.erase(cur);

      // HACK: Skip if this CE is a cast of the function in function call
      // (some tools handle this differently)
      if (auto *Call = dyn_cast<CallInst>(I)) {
        if (Call->getCalledOperand() == CE)
          continue;
      }

      auto *newI = CE->getAsInstruction();
      newI->insertBefore(I);
      I->replaceUsesOfWith(CE, newI);
      // The instruction may contain another CE
      queueCEInst(instsWithCE, *I);
      // The new instruction may contain CE
      queueCEInst(instsWithCE, *newI);

      changed = true;
    }
  }

  return changed;
}

} // namespace transform
} // namespace verification
} // namespace lotus

static llvm::RegisterPass<
    lotus::verification::transform::RemoveConstantExprsPass>
    X("remove-constant-exprs",
      "Transform constant expressions to instructions");

namespace lotus {
namespace verification {
namespace transform {

llvm::Pass *createRemoveConstantExprsPass() {
  return new RemoveConstantExprsPass();
}

} // namespace transform
} // namespace verification
} // namespace lotus
