#include "Verification/Transform/FlattenLoops.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"

#include <set>
#include <vector>

using namespace llvm;

namespace {

bool CloneMetadata(const Instruction *, Instruction *) {
  // Metadata cloning handled elsewhere if needed
  return false;
}

} // namespace

namespace lotus {
namespace verification {
namespace transform {

char FlattenLoopsPass::ID = 0;

bool FlattenLoopsPass::runOnLoop(Loop *L, LPPassManager & /*LPM*/) {
  auto *PL = L->getParentLoop();
  if (!PL)
    return false;

  auto *parentheader = PL->getHeader();
  auto *innerheader = L->getHeader();
  auto *fun = parentheader->getParent();
  auto &M = *fun->getParent();
  auto &Ctx = M.getContext();

  BasicBlock *flaginit =
      BasicBlock::Create(Ctx, "flatten.init", fun, parentheader);
  BasicBlock *newheaderbb =
      BasicBlock::Create(Ctx, "flatten.loop.header", fun, parentheader);

  // Flag whether we are in the inner or outer loop
  // Initialize "inner" to 0
  auto &entrybb = fun->getEntryBlock();
  auto *allocaTy = Type::getInt8Ty(Ctx);
  AllocaInst *flag =
      new AllocaInst(allocaTy, 0, nullptr,
                     M.getDataLayout().getPrefTypeAlign(allocaTy), "inner");
  flag->insertBefore(&*entrybb.getFirstInsertionPt());
  auto *SI = new StoreInst(ConstantInt::get(Type::getInt8Ty(Ctx), 0), flag,
                           /*isVolatile=*/false, flag->getAlign());
  SI->insertAfter(flag);
  BranchInst::Create(newheaderbb, flaginit);

  SmallVector<BasicBlock *, 2> exits;
  L->getExitBlocks(exits);
  for (auto *outbb : exits) {
    // Set inner to 0
    new StoreInst(ConstantInt::get(Type::getInt8Ty(Ctx), 0), flag,
                  &*outbb->getFirstInsertionPt());
  }

  // Redirect edges that jump to the inner header
  std::set<BranchInst *> jumpsToHeader;
  for (auto *pred : predecessors(innerheader)) {
    jumpsToHeader.insert(cast<BranchInst>(pred->getTerminator()));
  }

  for (auto *pBI : jumpsToHeader) {
    unsigned idx = 0;
    for (auto *succ : pBI->successors()) {
      if (succ == innerheader) {
        pBI->setSuccessor(idx, newheaderbb);
        // Set inner flag to true
        if (!L->contains(pBI->getParent())) {
          new StoreInst(ConstantInt::get(Type::getInt8Ty(Ctx), 1), flag, pBI);
        }
      }
      ++idx;
    }
  }

  // Redirect edges that go to parent header to the new header
  std::set<BranchInst *> jumpsToParentHeader;
  std::set<BranchInst *> backedgesToParentHeader;
  for (auto *pred : predecessors(parentheader)) {
    if (PL->contains(pred))
      backedgesToParentHeader.insert(cast<BranchInst>(pred->getTerminator()));
    else
      jumpsToParentHeader.insert(cast<BranchInst>(pred->getTerminator()));
  }

  for (auto *pBI : jumpsToParentHeader) {
    unsigned idx = 0;
    for (auto *succ : pBI->successors()) {
      if (succ == parentheader) {
        pBI->setSuccessor(idx, flaginit);
      }
      ++idx;
    }
  }

  for (auto *pBI : backedgesToParentHeader) {
    unsigned idx = 0;
    for (auto *succ : pBI->successors()) {
      if (succ == parentheader) {
        pBI->setSuccessor(idx, newheaderbb);
      }
      ++idx;
    }
  }

  // Create the branch instruction
  auto *LI = new LoadInst(Type::getInt8Ty(Ctx), flag, "innerval", newheaderbb);
  auto *Cmp = new ICmpInst(ICmpInst::ICMP_EQ,
                           ConstantInt::get(Type::getInt8Ty(Ctx), 1), LI);
  Cmp->insertAfter(LI);
  BranchInst::Create(innerheader, parentheader, Cmp, newheaderbb);

  // Update LoopInfo
  LoopInfo &loopinfo = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  PL->removeChildLoop(L);
  PL->addBasicBlockToLoop(newheaderbb, loopinfo);
  PL->moveToHeader(newheaderbb);

  errs() << "Flattened a loop with flag " << *flag << "\n";
  return true;
}

} // namespace transform
} // namespace verification
} // namespace lotus

static llvm::RegisterPass<lotus::verification::transform::FlattenLoopsPass>
    X("flatten-loops", "Flatten nested loops into non-nested loops");

namespace lotus {
namespace verification {
namespace transform {

llvm::Pass *createFlattenLoopsPass() { return new FlattenLoopsPass(); }

} // namespace transform
} // namespace verification
} // namespace lotus
