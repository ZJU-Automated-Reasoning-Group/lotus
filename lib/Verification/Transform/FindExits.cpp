#include "Verification/Transform/FindExits.h"

#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

static cl::opt<bool>
    no_change_assumes("no-change-assumes",
                      cl::desc("Do not replace __VERIFIER_assume with "
                               "__INSTR_check_assume"),
                      cl::init(false));

static cl::opt<bool>
    use_exit("find-exits-use-exit",
             cl::desc("Use calls to __VERIFIER_exit() instead of "
                      "__VERIFIER_silent_exit"),
             cl::init(false));

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

char FindExitsPass::ID = 0;

bool FindExitsPass::runOnFunction(Function &F) {
  if (F.isDeclaration())
    return false;

  bool modified = false;
  Module *M = F.getParent();
  LLVMContext &Ctx = M->getContext();
  bool isMain = F.getName().equals("main");

  Type *argTy = Type::getInt32Ty(Ctx);
  FunctionCallee exitC;
  if (use_exit) {
    exitC =
        M->getOrInsertFunction("__VERIFIER_exit", Type::getVoidTy(Ctx), argTy);
  } else {
    exitC = M->getOrInsertFunction("__VERIFIER_silent_exit",
                                   Type::getVoidTy(Ctx), argTy);
  }
  Function *exitF = cast<Function>(exitC.getCallee());
  exitF->addFnAttr(Attribute::NoReturn);

  for (auto &B : F) {
    // Find exit blocks (no successors)
    if (succ_begin(&B) == succ_end(&B)) {
      auto &BI = B.back();
      if (isMain || !isa<ReturnInst>(&BI)) {
        // Return inst does not abort the program (unless in main)
        auto *new_CI = CallInst::Create(exitF, {ConstantInt::get(argTy, 0)});
        CloneMetadata(&BI, new_CI);
        new_CI->insertBefore(&BI);
        modified = true;
      }
    }

    if (no_change_assumes)
      continue;

    // Change __VERIFIER_assume for __INSTR_check_assume
    for (auto &I : B) {
      if (auto *CI = dyn_cast<CallInst>(&I)) {
        auto *calledFun =
            dyn_cast<Function>(CI->getCalledOperand()->stripPointerCasts());
        if (!calledFun)
          continue;
        if (calledFun->getName().equals("__VERIFIER_assume")) {
          auto ICAC = M->getOrInsertFunction("__INSTR_check_assume",
                                             Type::getVoidTy(Ctx), argTy);
          auto *ICA = cast<Function>(ICAC.getCallee());
          CI->setCalledFunction(ICA);
          modified = true;
        }
      }
    }
  }

  return modified;
}

} // namespace transform
} // namespace verification
} // namespace lotus

static llvm::RegisterPass<lotus::verification::transform::FindExitsPass>
    X("find-exits",
      "Put calls to __VERIFIER_silent_exit into bitcode before any exit");

namespace lotus {
namespace verification {
namespace transform {

llvm::Pass *createFindExitsPass() { return new FindExitsPass(); }

} // namespace transform
} // namespace verification
} // namespace lotus
