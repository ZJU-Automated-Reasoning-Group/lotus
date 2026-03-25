#include "Verification/Transform/MakeNondet.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"

#include <set>
#include <string>

using namespace llvm;

static cl::opt<std::string> MakeNondetTargets(
    "make-nondet-targets",
    cl::desc("Comma-separated list of input functions "
             "to replace with nondet (default: rand,getchar,fgetc)"),
    cl::init("rand,getchar,fgetc"));

namespace lotus {
namespace verification {
namespace transform {

char MakeNondetPass::ID = 0;

static std::set<std::string> parseTargets(StringRef csv) {
  std::set<std::string> out;
  SmallVector<StringRef, 8> tokens;
  csv.split(tokens, ',', -1, false);
  for (StringRef t : tokens) {
    const StringRef s = t.trim();
    if (!s.empty())
      out.insert(s.str());
  }
  return out;
}

bool MakeNondetPass::runOnModule(Module &M) {
  const std::set<std::string> targets = parseTargets(MakeNondetTargets);
  if (targets.empty())
    return false;

  bool changed = false;
  SmallVector<CallInst *, 32> toReplace;

  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    for (BasicBlock &BB : F) {
      for (Instruction &I : BB) {
        auto *CI = dyn_cast<CallInst>(&I);
        if (!CI || CI->isInlineAsm())
          continue;
        Function *Callee = CI->getCalledFunction();
        if (!Callee || !Callee->hasName())
          continue;
        if (targets.find(Callee->getName().str()) == targets.end())
          continue;
        if (CI->getType()->isVoidTy())
          continue;
        toReplace.push_back(CI);
      }
    }
  }

  for (CallInst *CI : toReplace) {
    Type *RetTy = CI->getType();
    IRBuilder<> B(CI);

    std::string NondetName = "verifier.nondet.input.";
    if (RetTy->isIntegerTy())
      NondetName += "i" + std::to_string(RetTy->getIntegerBitWidth());
    else if (RetTy->isFloatingPointTy())
      NondetName += "fp";
    else if (RetTy->isPointerTy())
      NondetName += "ptr";
    else
      NondetName += "val";

    FunctionCallee ND =
        M.getOrInsertFunction(NondetName, FunctionType::get(RetTy, false));
    Value *V = B.CreateCall(ND);
    CI->replaceAllUsesWith(V);
    CI->eraseFromParent();
    changed = true;
  }

  return changed;
}

} // namespace transform
} // namespace verification
} // namespace lotus

static llvm::RegisterPass<lotus::verification::transform::MakeNondetPass>
    Y("make-nondet",
      "Replace selected input functions with nondeterministic values");

namespace lotus {
namespace verification {
namespace transform {

llvm::Pass *createMakeNondetPass() { return new MakeNondetPass(); }

} // namespace transform
} // namespace verification
} // namespace lotus
