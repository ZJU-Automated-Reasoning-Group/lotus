#include "Verification/Transform/DummyMarker.h"

#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"

using namespace llvm;

namespace {

bool CloneMetadata(const Instruction *i1, Instruction *i2) {
  if (i1->hasMetadata()) {
    i2->setDebugLoc(i1->getDebugLoc());
    return true;
  }
  return false;
}

} // namespace

namespace lotus {
namespace verification {
namespace transform {

char DummyMarkerPass::ID = 0;

bool DummyMarkerPass::runOnFunction(Function &F) {
  if (F.isDeclaration())
    return false;

  bool modified = false;
  Module *M = F.getParent();
  LLVMContext &Ctx = M->getContext();

  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E;) {
    Instruction *ins = &*I;
    ++I;
    auto *CI = dyn_cast<CallInst>(ins);
    if (!CI)
      continue;

    auto *calledFun =
        dyn_cast<Function>(CI->getCalledOperand()->stripPointerCasts());
    if (!calledFun)
      continue;
    auto fun = calledFun->getName();
    if (fun.equals("malloc") || fun.equals("calloc")) {
      auto dummyC =
          M->getOrInsertFunction("__symbiotic_keep_ptr", Type::getVoidTy(Ctx),
                                 Type::getInt8PtrTy(Ctx));
      auto *dummy = cast<Function>(dummyC.getCallee());
      auto *new_CI = CallInst::Create(dummy, {CI});
      CloneMetadata(CI, new_CI);

      new_CI->insertAfter(CI);
      modified = true;
    }
  }
  return modified;
}

} // namespace transform
} // namespace verification
} // namespace lotus

static llvm::RegisterPass<lotus::verification::transform::DummyMarkerPass>
    X("dummy-marker",
      "Put calls to dummy functions into bitcode to prevent code removal");

namespace lotus {
namespace verification {
namespace transform {

llvm::Pass *createDummyMarkerPass() { return new DummyMarkerPass(); }

} // namespace transform
} // namespace verification
} // namespace lotus
