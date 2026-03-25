#include "Verification/Transform/DeleteUndefined.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"

#include <set>
#include <unordered_set>

using namespace llvm;

namespace {

static const char *LeaveCalls[] = {"__VERIFIER_error",
                                   "__VERIFIER_assume",
                                   "__VERIFIER_assert",
                                   "__assert_fail",
                                   "abort",
                                   "exit",
                                   "malloc",
                                   "calloc",
                                   "realloc",
                                   "free",
                                   "memset",
                                   "memcmp",
                                   "memcpy",
                                   "memmove",
                                   nullptr};

static bool shouldLeaveCall(StringRef Name) {
  if (Name.startswith("__VERIFIER_"))
    return true;
  for (const char **Curr = LeaveCalls; *Curr; ++Curr) {
    if (Name.equals(*Curr))
      return true;
  }
  return false;
}

} // namespace

namespace lotus {
namespace verification {
namespace transform {

char DeleteUndefinedPass::ID = 0;

static void defineAsNondet(Module &M, Function *F) {
  assert(F->isDeclaration() && !F->getReturnType()->isVoidTy());

  LLVMContext &Ctx = M.getContext();
  Type *RetTy = F->getReturnType();

  // Create function body
  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", F);
  IRBuilder<> B(Entry);

  // Create nondet function name
  std::string NondetName = "verifier.nondet.undef.";
  if (RetTy->isIntegerTy())
    NondetName += "i" + std::to_string(RetTy->getIntegerBitWidth());
  else if (RetTy->isFloatingPointTy())
    NondetName += "fp";
  else if (RetTy->isPointerTy())
    NondetName += "ptr";
  else
    NondetName += "val";

  FunctionCallee NondetFn =
      M.getOrInsertFunction(NondetName, FunctionType::get(RetTy, false));
  Value *NondetVal = B.CreateCall(NondetFn);
  B.CreateRet(NondetVal);

  F->setLinkage(GlobalValue::InternalLinkage);
}

bool DeleteUndefinedPass::runOnModule(Module &M) {
  bool Changed = false;
  std::unordered_set<Function *> Defined;

  // First pass: define undefined functions that return values
  for (Function &F : M) {
    if (F.isIntrinsic() || F.isDeclaration())
      continue;
    if (shouldLeaveCall(F.getName()))
      continue;
    if (!F.empty())
      Defined.insert(&F);
  }

  // Second pass: delete calls to undefined void functions
  for (Function &F : M) {
    if (F.isDeclaration() || F.isIntrinsic())
      continue;

    SmallVector<CallInst *, 32> ToDelete;
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        auto *CI = dyn_cast<CallInst>(&I);
        if (!CI || CI->isInlineAsm())
          continue;

        Function *Callee = CI->getCalledFunction();
        if (!Callee || Callee->isIntrinsic())
          continue;
        if (shouldLeaveCall(Callee->getName()))
          continue;
        if (Defined.count(Callee))
          continue;
        if (!Callee->isDeclaration())
          continue;
        if (!CI->getType()->isVoidTy())
          continue;

        ToDelete.push_back(CI);
      }
    }

    for (CallInst *CI : ToDelete) {
      CI->eraseFromParent();
      Changed = true;
    }
  }

  // Third pass: replace undefined non-void functions with nondet
  for (Function &F : M) {
    if (F.isIntrinsic() || !F.isDeclaration())
      continue;
    if (shouldLeaveCall(F.getName()))
      continue;
    if (F.getReturnType()->isVoidTy())
      continue;

    defineAsNondet(M, &F);
    Changed = true;
  }

  return Changed;
}

} // namespace transform
} // namespace verification
} // namespace lotus

static llvm::RegisterPass<lotus::verification::transform::DeleteUndefinedPass>
    Y("delete-undefined",
      "Delete calls to undefined functions, replace with nondet");

namespace lotus {
namespace verification {
namespace transform {

llvm::Pass *createDeleteUndefinedPass() { return new DeleteUndefinedPass(); }

} // namespace transform
} // namespace verification
} // namespace lotus
