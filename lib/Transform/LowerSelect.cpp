// LowerSelect pass converts select instructions to explicit if/else control
// flow. This transformation can help with analysis passes that have difficulty
// handling conditional expressions.

#include "Transform/LowerSelect.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/Debug.h>

#define DEBUG_TYPE "lower-select"

// Transform a select instruction into explicit if/else control flow.
// Creates two new basic blocks for the true and false cases, connected by
// a conditional branch from the original block.
static void transform(Instruction *Select) {
  auto *Function = Select->getFunction();
  auto *BranchBB = Select->getParent();

  // Note that all instructions BEFORE the specified iterator
  // stay as part of the original basic block, an unconditional branch is added
  // to the original BB, and the rest of the instructions in the BB are moved
  // to the new BB, including the old terminator.  The newly formed basic block
  // is returned. This function invalidates the specified iterator.
  BasicBlock *PhiBB = Select->getParent()->splitBasicBlock(Select, "");

  auto *TrueBB = BasicBlock::Create(Select->getContext(), "", Function, PhiBB);
  BranchInst::Create(PhiBB, TrueBB);
  auto *FalseBB = BasicBlock::Create(Select->getContext(), "", Function, PhiBB);
  BranchInst::Create(PhiBB, FalseBB);

  BranchBB->getTerminator()->eraseFromParent();
  BranchInst::Create(TrueBB, FalseBB, Select->getOperand(0), BranchBB);

  auto *Phi = PHINode::Create(Select->getType(), 2, "", Select);
  Phi->addIncoming(Select->getOperand(1), TrueBB);
  Phi->addIncoming(Select->getOperand(2), FalseBB);
  Select->replaceAllUsesWith(Phi);
  Select->eraseFromParent();
}

// New Pass Manager entry point. Finds all select instructions (excluding
// pointer selects) and transforms them into explicit if/else control flow.
PreservedAnalyses LowerSelectPass::run(Module &M, ModuleAnalysisManager &) {
  std::vector<SelectInst *> Selects;
  for (auto &F : M) {
    for (auto &B : F) {
      for (auto &I : B) {
        if (auto *SI = dyn_cast<SelectInst>(&I)) {
          if (SI->getType()->isPointerTy())
            continue;
          // Lowering below creates a conditional branch and therefore only
          // supports scalar i1 conditions.
          if (!SI->getCondition()->getType()->isIntegerTy(1))
            continue;
          Selects.push_back(SI);
        }
      }
    }
  }
  if (Selects.empty())
    return PreservedAnalyses::all();

  for (auto *Select : Selects)
    transform(Select);

  if (verifyModule(M, &errs())) {
    llvm_unreachable("Error: Lowerselect fails...");
  }
  // CFG is modified, invalidate most analyses
  return PreservedAnalyses::none();
}
