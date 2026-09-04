/// A pass to replace all variadic functions by their declarations

#include "Transform/KillVarArgFn.h"

#include "llvm/IR/Module.h"
#include "llvm/Pass.h"

namespace {
using namespace llvm;
class KillVarArgFn : public ModulePass {
public:
  static char ID;
  KillVarArgFn() : ModulePass(ID) {}

  bool runOnModule(Module &M) override { return killVarArgFunctions(M); }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesAll();
  }
};
char KillVarArgFn::ID = 0;
} // namespace

namespace seahorn {
Pass *createKillVarArgFnPass() { return new KillVarArgFn(); }
} // namespace seahorn

static llvm::RegisterPass<KillVarArgFn> X("kill-vaarg",
                                          "Remove variadic functions");
