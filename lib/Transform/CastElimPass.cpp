#include "Transform/CastElimPass.h"

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/Debug.h>

void CastElimPass::getAnalysisUsage(llvm::AnalysisUsage &AU) const {
  AU.setPreservesCFG();
}

namespace {
llvm::Value *findSubstitute(llvm::CastInst &CI) {
  using llvm::CastInst;
  llvm::Value *src = CI.getOperand(0);
  if (auto *csrc = llvm::dyn_cast<CastInst>(src)) { /* A->B->C cast */
    if (csrc->getSrcTy() == CI.getDestTy()          /* A->B->A cast */
        && CI.getOpcode() == CastInst::Trunc &&
        (csrc->getOpcode() == CastInst::SExt ||
         csrc->getOpcode() == CastInst::ZExt)) {
      return csrc->getOperand(0);
    }
  }
  return nullptr;
}
} // namespace

bool CastElimPass::runOnFunction(llvm::Function &F) {
  size_t eliminated = 0;

  for (auto BBI = F.begin(); BBI != F.end(); ++BBI) {
    for (auto it = BBI->begin(), end = std::prev(BBI->end()); it != end; ++it) {
      if (auto *CI = llvm::dyn_cast<llvm::CastInst>(it)) {
        if (llvm::Value *S = findSubstitute(*CI)) {
          CI->replaceAllUsesWith(S);
          ++eliminated;
        }
      }
    }
  }

  // if (eliminated != 0)
  //   llvm::dbgs() << "Eliminated " << std::to_string(eliminated)
  //                << " casts from " << F.getName() << "\n";
  return eliminated != 0;
}

char CastElimPass::ID = 0;
static llvm::RegisterPass<CastElimPass> X("cast-elim",
                                          "Eliminate some unnecessary casts.");