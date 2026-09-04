#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"

#include "seahorn/Transforms/Kernel/Util.hh"

using namespace llvm;

namespace seahorn {

class HandleDevm : public ModulePass {
public:
  static char ID;

  HandleDevm() : ModulePass(ID) {}

  bool runOnModule(Module &m) override {
    handleDevresAdd(m);
    return true;
  }

  virtual StringRef getPassName() const override { return "Devm"; }

private:
  void handleDevresAdd(Module &m) {
    Function *devresAdd = m.getFunction("drvhorn.devres_add");
    if (!devresAdd)
      return;
    Function *alloc = getOrCreateAlloc(m);
    for (CallInst *call : getCalls(devresAdd)) {
      CallInst *devresAlloc =
          dyn_cast<CallInst>(call->getArgOperand(1)->stripPointerCasts());
      if (!devresAlloc) {
        errs() << "TODO: devres_add's 2nd argument is not devres_alloc?\n";
        continue;
      }
      ConstantInt *size = dyn_cast<ConstantInt>(devresAlloc->getArgOperand(1));
      if (!size) {
        continue;
      }
      Function *release = dyn_cast<Function>(
          devresAlloc->getArgOperand(0)->stripPointerCasts());
      if (!release)
        continue;

      IRBuilder<> b(devresAlloc);
      Value *devresReplace = b.CreateCall(alloc, size, "devres");
      devresAlloc->replaceAllUsesWith(devresReplace);
      devresAlloc->eraseFromParent();

      b.SetInsertPoint(call);
      b.CreateCall(release,
                   {Constant::getNullValue(release->getArg(0)->getType()),
                    devresReplace});
      call->eraseFromParent();
    }
  }
};

char HandleDevm::ID = 0;

Pass *createHandleDevmPass() { return new HandleDevm(); }
} // namespace seahorn