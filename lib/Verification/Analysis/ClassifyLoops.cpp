#include "Verification/Analysis/ClassifyLoops.h"

#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/LoopIterator.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace lotus {
namespace verification {
namespace analysis {

char ClassifyLoopsPass::ID = 0;

static bool any = false;
static bool nested = false;
static bool nonterm = false;
static bool irreducible = false;

bool ClassifyLoopsPass::runOnLoop(Loop *L, LPPassManager & /*LPM*/) {
  any = true;

  // Detect nested loops
  if (L->getParentLoop()) {
    nested = true;
  }

  // Detect non-terminating loops
  if (!nonterm) {
    SmallVector<BasicBlock *, 8> ExitBlocks;
    L->getExitBlocks(ExitBlocks);
    if (ExitBlocks.size() == 0) {
      nonterm = true;
    }
  }

  // Detect irreducible loops
  if (!irreducible) {
    LoopInfo &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
    LoopBlocksRPO RPOT(L);
    RPOT.perform(&LI);
    irreducible = containsIrreducibleCFG<const BasicBlock *>(RPOT, LI);
  }

  return false;
}

bool ClassifyLoopsPass::doFinalization() {
  if (any) {
    errs() << "contains loops\n";
    if (nested)
      errs() << "  nested loops\n";
    if (nonterm)
      errs() << "  nonterm loops\n";
    if (irreducible)
      errs() << "  irreducible loops\n";
  }
  // Reset for next module
  any = nested = nonterm = irreducible = false;
  return false;
}

} // namespace analysis
} // namespace verification
} // namespace lotus

static llvm::RegisterPass<lotus::verification::analysis::ClassifyLoopsPass>
    X("classify-loops", "Detect what loops are in the program");

namespace lotus {
namespace verification {
namespace analysis {

llvm::Pass *createClassifyLoopsPass() { return new ClassifyLoopsPass(); }

} // namespace analysis
} // namespace verification
} // namespace lotus
