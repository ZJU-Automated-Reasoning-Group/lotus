#include "Transform/StripLifetime.h"

#include "llvm/IR/Module.h"
#include "llvm/Pass.h"

using namespace llvm;

namespace {
class StripLifetime : public ModulePass {
public:
  static char ID;
  StripLifetime() : ModulePass(ID) {}

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
  }

  bool runOnModule(Module &M) override { return stripLifetimeIntrinsics(M); }
};
char StripLifetime::ID = 0;
} // namespace

namespace seahorn {
Pass *createStripLifetimePass() { return new StripLifetime(); }
} // namespace seahorn

static llvm::RegisterPass<StripLifetime> Y("strip-lifetime",
                                           "Remove llvm.lifetime intrinsics");


