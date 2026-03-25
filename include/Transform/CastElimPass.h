#ifndef __CAST_ELIM_PASS_H__
#define __CAST_ELIM_PASS_H__

#include <llvm/Analysis/LoopPass.h>
#include <llvm/Pass.h>

/* The CastElim pass eliminates some unnecessary casts that can
 * complicate later analyses. */
class CastElimPass final : public llvm::FunctionPass {
public:
  static char ID;
  CastElimPass() : llvm::FunctionPass(ID) {}
  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;
  bool runOnFunction(llvm::Function &F) override;
  llvm::StringRef getPassName() const override { return "CastElimPass"; }

private:
};

#endif