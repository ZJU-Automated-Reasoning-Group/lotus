#include "Verification/Transform/BreakCritLoops.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;

namespace {

bool CloneMetadata(const Instruction *src, Instruction *dst) {
  if (src->hasMetadata()) {
    dst->copyMetadata(*src);
    return true;
  }
  return false;
}

} // namespace

namespace lotus {
namespace verification {
namespace transform {

char BreakCritLoopsPass::ID = 0;

bool BreakCritLoopsPass::runOnFunction(Function &F) {
  if (F.isDeclaration())
    return false;

  std::vector<BasicBlock *> toProcess;

  for (BasicBlock &BB : F) {
    if (BB.size() <= 1)
      continue;

    auto *Term = BB.getTerminator();
    if (auto *BI = dyn_cast<BranchInst>(Term)) {
      if (BI->isUnconditional())
        continue;

      for (BasicBlock *SuccBB : BI->successors()) {
        BasicBlock *UniqueSucc = SuccBB->getUniqueSuccessor();
        if (UniqueSucc && UniqueSucc == &BB) {
          toProcess.push_back(&BB);
          break;
        }
      }
    }
  }

  bool Changed = false;
  for (BasicBlock *BB : toProcess) {
    BasicBlock::iterator SplitPoint = --BB->end();
    BasicBlock *NewBB = BB->splitBasicBlock(SplitPoint, "crit.blk.split");
    if (!CloneMetadata(BB->getTerminator(), BB->getTerminator())) {
      errs() << "[BreakCritLoops] Failed assigning metadata\n";
    }
    Changed = true;
  }

  return Changed;
}

} // namespace transform
} // namespace verification
} // namespace lotus

static llvm::RegisterPass<lotus::verification::transform::BreakCritLoopsPass>
    X("break-crit-loops", "Break critical loops for better slicing");

namespace lotus {
namespace verification {
namespace transform {

llvm::Pass *createBreakCritLoopsPass() { return new BreakCritLoopsPass(); }

} // namespace transform
} // namespace verification
} // namespace lotus
