#include "Verification/Transform/ReplaceLifetimeMarkers.h"

#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
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

char ReplaceLifetimeMarkersPass::ID = 0;

bool ReplaceLifetimeMarkersPass::runOnFunction(Function &F) {
  if (F.isDeclaration())
    return false;

  bool modified = false;
  Module *M = F.getParent();
  LLVMContext &Ctx = M->getContext();
  auto ver_scope_enterC = M->getOrInsertFunction(
      "__VERIFIER_scope_enter", Type::getVoidTy(Ctx), Type::getInt8PtrTy(Ctx));
  auto ver_scope_leaveC = M->getOrInsertFunction(
      "__VERIFIER_scope_leave", Type::getVoidTy(Ctx), Type::getInt8PtrTy(Ctx));
  auto *ver_scope_enter = cast<Function>(ver_scope_enterC.getCallee());
  auto *ver_scope_leave = cast<Function>(ver_scope_leaveC.getCallee());

  for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E;) {
    Instruction *ins = &*I;
    ++I;
    if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(ins)) {
      if (II->getIntrinsicID() != Intrinsic::lifetime_start &&
          II->getIntrinsicID() != Intrinsic::lifetime_end)
        continue;

      CallInst *CI = nullptr;
      if (II->getIntrinsicID() == Intrinsic::lifetime_start) {
        CI = CallInst::Create(ver_scope_enter, {II->getOperand(1)});
      } else if (II->getIntrinsicID() == Intrinsic::lifetime_end) {
        CI = CallInst::Create(ver_scope_leave, {II->getOperand(1)});
      }

      CloneMetadata(II, CI);
      CI->insertAfter(II);
      II->eraseFromParent();
      modified = true;
    }
  }
  return modified;
}

} // namespace transform
} // namespace verification
} // namespace lotus

static llvm::RegisterPass<
    lotus::verification::transform::ReplaceLifetimeMarkersPass>
    X("replace-lifetime-markers",
      "Replace lifetime markers with calls to __VERIFIER_scope_*");

namespace lotus {
namespace verification {
namespace transform {

llvm::Pass *createReplaceLifetimeMarkersPass() {
  return new ReplaceLifetimeMarkersPass();
}

} // namespace transform
} // namespace verification
} // namespace lotus
