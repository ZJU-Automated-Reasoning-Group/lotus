// Clone-and-wrap transformation for modular trimming (paper §5, Interprocedural
// instrumentation).
//
// Paper: create prc' (safe clone) with assert→assume and calls→prc'; at each
// call site replace
//   v := call prc(e)  by  if(⋆) { v := call prc'(e) } else { v := call prc(e);
//   assume false }.
// This allows local trimming assumptions inside prc while preserving failing
// executions.

#include "FailureDirectedTrimmingImpl.h"

#include <llvm/ADT/STLExtras.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Transforms/Utils/Cloning.h>

using namespace llvm;

static bool passesNameFiltersForSafeClone(const Function &F) {
  // Safe clones are intended to represent "cannot fail" executions.
  // We exclude verifier intrinsics/annotations and other special functions from
  // cloning to avoid interfering with the verification/runtime API.
  if (F.isDeclaration())
    return false;
  StringRef Name = F.getName();
  if (Name.startswith("verifier.") || Name.startswith("__CRAB_") ||
      Name.startswith("__VERIFIER_") || Name.startswith("llvm."))
    return false;
  if (isAssumeFunctionName(Name) || isErrorFunctionName(Name) ||
      isAssumeNotFunctionName(Name) || isAssertFunctionName(Name) ||
      isNondetFunctionName(Name))
    return false;
  if (Name.endswith(".fdtrim.safe"))
    return false;
  return true;
}

// Paper §5: create prc' for each prc with assert φ → assume φ, error → assume
// false; replace calls to prc inside prc' with calls to prc'.
DenseMap<Function *, Function *> cloneSafeFunctions(Module &M,
                                                    FunctionCallee AssumeFn) {
  // Safe clone f.fdtrim.safe: cannot exhibit assertion failure (assert→assume,
  // error→assume(false)). After wrapCallsInOriginalFunctions, executions either
  // follow safe clones or enter failure context.
  DenseMap<Function *, Function *> SafeOf;

  DenseSet<Function *> Candidates;
  for (Function &F : M) {
    if (!passesNameFiltersForSafeClone(F))
      continue;

    // The safe clone transformation below assumes simple direct calls.
    // If a function contains indirect calls, invoke/callbr, or inline asm, we
    // conservatively skip it to avoid changing exception edges or unknown
    // side effects.
    bool Unsupported = false;
    for (Instruction &I : instructions(F)) {
      if (isa<InvokeInst>(&I) || isa<CallBrInst>(&I)) {
        Unsupported = true;
        break;
      }
      auto *CB = dyn_cast<CallBase>(&I);
      if (!CB)
        continue;
      if (CB->isInlineAsm()) {
        Unsupported = true;
        break;
      }
      if (!getDirectCalledFunctionMatchingType(*CB)) {
        Unsupported = true;
        break;
      }
    }
    if (Unsupported)
      continue;
    Candidates.insert(&F);
  }

  // Ensure safe-clone candidates are closed under direct calls to defined
  // functions: if a candidate calls a defined non-special function that we
  // can't safely clone, the candidate itself can't be made safe.
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (Function *F : llvm::make_early_inc_range(Candidates)) {
      bool Remove = false;
      for (Instruction &I : instructions(*F)) {
        auto *CB = dyn_cast<CallBase>(&I);
        if (!CB)
          continue;
        Function *CF = getDirectCalledFunctionMatchingType(*CB);
        if (!CF)
          continue;

        if (CF->isDeclaration())
          continue;
        if (!passesNameFiltersForSafeClone(*CF))
          continue;
        if (!Candidates.count(CF)) {
          Remove = true;
          break;
        }
      }
      if (Remove) {
        Candidates.erase(F);
        Changed = true;
      }
    }
  }

  for (Function *F : Candidates) {
    ValueToValueMapTy VMap;
    Function *Clone = CloneFunction(F, VMap);
    Clone->setName(F->getName() + ".fdtrim.safe");
    SafeOf[F] = Clone;
  }

  for (auto &KV : SafeOf) {
    Function *Safe = KV.second;

    std::vector<CallInst *> CallsToRewrite;
    for (Instruction &I : instructions(Safe)) {
      if (auto *CI = dyn_cast<CallInst>(&I)) {
        if (!getDirectCalledFunctionMatchingType(*CI))
          continue;
        CallsToRewrite.push_back(CI);
      }
    }

    for (CallInst *CI : CallsToRewrite) {
      Function *CF = getDirectCalledFunctionMatchingType(*CI);
      if (!CF)
        continue;

      if (isErrorFunctionName(CF->getName())) {
        // error() in the safe clone becomes assume(false): block the path.
        IRBuilder<> B(CI);
        Value *False = ConstantInt::getFalse(M.getContext());
        B.CreateCall(AssumeFn, False);
        CI->eraseFromParent();
        continue;
      }

      if (isAssertFunctionName(CF->getName())) {
        // assert(c) in the safe clone becomes assume(c): the safe clone cannot
        // fail via this check.
        IRBuilder<> B(CI);
        Value *CondV = nullptr;
        if (CI->arg_size() >= 1)
          CondV = CI->getArgOperand(0);
        if (!CondV) {
          CondV = ConstantInt::getTrue(M.getContext());
        } else if (CondV->getType()->isIntegerTy(1)) {
          // ok
        } else if (CondV->getType()->isIntegerTy()) {
          CondV = B.CreateICmpNE(CondV, ConstantInt::get(CondV->getType(), 0));
        } else if (CondV->getType()->isPointerTy()) {
          CondV = B.CreateICmpNE(
              CondV,
              ConstantPointerNull::get(cast<PointerType>(CondV->getType())));
        } else {
          CondV = ConstantInt::getFalse(M.getContext());
        }
        B.CreateCall(AssumeFn, CondV);
        CI->eraseFromParent();
        continue;
      }

      if (isAssumeFunctionName(CF->getName()) ||
          isAssumeNotFunctionName(CF->getName()) ||
          isNondetFunctionName(CF->getName()))
        continue;

      // Rewrite regular direct calls to other instrumented functions to call
      // their safe clones.
      for (auto &MapEntry : SafeOf) {
        if (CF == MapEntry.first) {
          CI->setCalledFunction(MapEntry.second);
          break;
        }
      }
    }
  }

  return SafeOf;
}

// Paper §5: at call site v := call prc(e), replace with
//   if(⋆) v := call prc'(e) else { v := call prc(e); assume false };
//   unreachable on else.
// Failure branch ensures every entry to prc is followed by assume false
// (justifies local trimming).
bool wrapCallsInOriginalFunctions(Module &M, FunctionCallee AssumeFn,
                                  DenseMap<Function *, Function *> &SafeOf,
                                  NondetFactory &Nondet) {
  // For each call f(args) with safe clone: if (nondet) call f.safe(args) else {
  // call f(args); assume(false); unreachable }.
  bool Changed = false;

  for (Function &F : M) {
    if (F.isDeclaration())
      continue;
    if (F.getName().endswith(".fdtrim.safe"))
      continue;

    std::vector<CallInst *> Calls;
    for (Instruction &I : instructions(F)) {
      auto *CI = dyn_cast<CallInst>(&I);
      if (!CI)
        continue;
      Function *CF = getDirectCalledFunctionMatchingType(*CI);
      if (!CF)
        continue;
      if (isAssumeFunctionName(CF->getName()) ||
          isAssumeNotFunctionName(CF->getName()) ||
          isAssertFunctionName(CF->getName()) ||
          isErrorFunctionName(CF->getName()) ||
          isNondetFunctionName(CF->getName()))
        continue;
      if (!SafeOf.count(CF))
        continue;
      Calls.push_back(CI);
    }

    std::reverse(Calls.begin(), Calls.end());

    for (CallInst *CI : Calls) {
      if (!CI->getParent())
        continue;
      Function *CF = CI->getCalledFunction();
      if (!CF || !SafeOf.count(CF))
        continue;

      BasicBlock *OrigBB = CI->getParent();
      Function *Fn = OrigBB->getParent();

      BasicBlock *ContBB =
          OrigBB->splitBasicBlock(CI->getIterator(), "fdtrim.cont");
      OrigBB->getTerminator()->eraseFromParent();

      LLVMContext &Ctx = M.getContext();
      BasicBlock *SafeBB = BasicBlock::Create(Ctx, "fdtrim.safe", Fn, ContBB);
      BasicBlock *FailBB = BasicBlock::Create(Ctx, "fdtrim.fail", Fn, ContBB);

      IRBuilder<> B(OrigBB);
      Value *Nd = Nondet.nondetBool(B);
      B.CreateCondBr(Nd, SafeBB, FailBB);

      IRBuilder<> BS(SafeBB);
      SmallVector<Value *, 8> CallArgs;
      for (Use &U : CI->args())
        CallArgs.push_back(U.get());
      CallInst *SafeCall = BS.CreateCall(SafeOf[CF], CallArgs);
      SafeCall->setCallingConv(CI->getCallingConv());
      SafeCall->setTailCallKind(CI->getTailCallKind());
      SafeCall->setAttributes(CI->getAttributes());
      if (!CI->getType()->isVoidTy()) {
        CI->replaceAllUsesWith(SafeCall);
      }
      BS.CreateBr(ContBB);

      IRBuilder<> BF(FailBB);
      CallInst *OrigCall = BF.CreateCall(CF, CallArgs);
      OrigCall->setCallingConv(CI->getCallingConv());
      OrigCall->setTailCallKind(CI->getTailCallKind());
      OrigCall->setAttributes(CI->getAttributes());
      BF.CreateCall(AssumeFn, ConstantInt::getFalse(Ctx));
      BF.CreateUnreachable();

      CI->eraseFromParent();
      Changed = true;
    }
  }

  return Changed;
}
