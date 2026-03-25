// Name anonymous basic blocks with integer names, as GCC does per
// default. Makes them easier to describe.

#define DEBUG_TYPE "nameblocks"

#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {

class NameBlocksPass : public ModulePass {
public:
  static char ID;
  NameBlocksPass() : ModulePass(ID) {}

  bool runOnModule(Module &M);
  void runOnFunction(Function *F);

  virtual void getAnalysisUsage(AnalysisUsage &AU) const {};
};

} // namespace

char NameBlocksPass::ID = 0;
static RegisterPass<NameBlocksPass>
    X("nameblocks",
      "Name anonymous blocks with integers (i.e. <label>:%0 -> \"0\" or some "
      "other int)",
      false /* CFG only? */, false /* Is analysis? */);

namespace llvm {
Pass *createNameBlocksPass() { return new NameBlocksPass(); }
} // namespace llvm

void NameBlocksPass::runOnFunction(Function *F) {
  uint64_t NameInt = 0;
  std::string Name;

  for (BasicBlock &B : *F) {
    if (!B.hasName()) {
      Name.clear();
      {
        raw_string_ostream RSO(Name);
        RSO << (++NameInt);
      }
      B.setName(Name);
    }
  }
}

bool NameBlocksPass::runOnModule(Module &M) {
  for (Function &F : M) {
    runOnFunction(&F);
  }
  return true;
}
