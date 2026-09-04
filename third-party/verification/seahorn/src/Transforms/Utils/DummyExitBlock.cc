/** Insert dummy exit basic blocks */
#include "Transform/DummyExitBlock.h"

#include "llvm/IR/Function.h"
#include "llvm/Pass.h"

using namespace llvm;

namespace seahorn {
class DummyExitBlock : public FunctionPass {
public:
  static char ID;
  DummyExitBlock() : FunctionPass(ID) {}

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
  }

  bool runOnFunction(Function &F) override { return addDummyExitBlock(F); }
};

char DummyExitBlock::ID = 0;

Pass *createDummyExitBlockPass() { return new DummyExitBlock(); }
} // namespace seahorn

