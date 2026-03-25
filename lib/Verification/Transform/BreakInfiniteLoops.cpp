#include "Verification/Transform/BreakInfiniteLoops.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"

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

char BreakInfiniteLoopsPass::ID = 0;

bool BreakInfiniteLoopsPass::runOnLoop(Loop *L, LPPassManager & /*LPM*/) {
  SmallVector<BasicBlock *, 2> exits;
  L->getExitingBlocks(exits);
  if (!exits.empty())
    return false;

  // Found a syntactically infinite loop, like while(1){}
  // Break it by adding a conditional exit that never executes
  BasicBlock *header = L->getHeader();
  Module *M = header->getParent()->getParent();
  LLVMContext &Ctx = M->getContext();

  // Gather jumps to the header block
  std::vector<std::pair<BasicBlock *, unsigned>> to_change;
  for (auto I = pred_begin(header), E = pred_end(header); I != E; ++I) {
    auto *TI = (*I)->getTerminator();
    for (int i = 0, e = TI->getNumSuccessors(); i < e; ++i) {
      if (TI->getSuccessor(i) == header)
        to_change.emplace_back(*I, i);
    }
  }

  // Create exit block
  Function *F = header->getParent();
  BasicBlock *exitBB = BasicBlock::Create(Ctx, "inf.loop.exit", F);
  new UnreachableInst(Ctx, exitBB);

  // Create a new block with always-true condition
  GlobalVariable *trueGV = new GlobalVariable(
      *M, Type::getInt1Ty(Ctx), true /*constant*/,
      GlobalVariable::PrivateLinkage, ConstantInt::getTrue(Ctx), "always_true");

  BasicBlock *nb = BasicBlock::Create(Ctx, "break.inf.loop");
  LoadInst *LI = new LoadInst(Type::getInt1Ty(Ctx), trueGV, "always_true", nb);

  if (!CloneMetadata(header->getTerminator(), LI)) {
    errs() << "[BreakInfiniteLoops] Failed assigning metadata to: " << *LI
           << "\n";
  }

  auto *Br = BranchInst::Create(header, exitBB, LI, nb);
  if (!CloneMetadata(header->getTerminator(), Br)) {
    errs() << "[BreakInfiniteLoops] Failed assigning metadata to: " << *Br
           << "\n";
  }

  // Insert the new block before header
  nb->insertInto(F, header);

  // Change the jump instructions
  for (auto &pr : to_change) {
    auto *TI = pr.first->getTerminator();
    TI->setSuccessor(pr.second, nb);
  }

  // Update LoopInfo
  LoopInfo &LoopInfo = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  L->addBasicBlockToLoop(nb, LoopInfo);
  L->moveToHeader(nb);

  return true;
}

} // namespace transform
} // namespace verification
} // namespace lotus

static llvm::RegisterPass<
    lotus::verification::transform::BreakInfiniteLoopsPass>
    X("break-infinite-loops",
      "Transform loops that have no exit to loops that have an exit");

namespace lotus {
namespace verification {
namespace transform {

llvm::Pass *createBreakInfiniteLoopsPass() {
  return new BreakInfiniteLoopsPass();
}

} // namespace transform
} // namespace verification
} // namespace lotus
