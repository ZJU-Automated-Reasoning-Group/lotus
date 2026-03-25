/*
 * Inlining helpers for the ctllvm pass.
 */

#include "CTInternal.h"

#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"

#include <cassert>

using namespace llvm;

namespace ctllvm {
namespace detail {

void CryptoAnalysisImpl::updateSecureFunctionNames() {
  secure_function_names_.insert("fprintf");
  secure_function_names_.insert("fopen");
  secure_function_names_.insert("fputc");
  secure_function_names_.insert("malloc");
  secure_function_names_.insert("calloc");
  secure_function_names_.insert("memset");
  secure_function_names_.insert("free");
  secure_function_names_.insert("explicit_bzero");
  secure_function_names_.insert("abort");
  secure_function_names_.insert("exit");
}

AnalysisError
CryptoAnalysisImpl::getFunctionCalls(Function &F,
                                     std::set<Function *> &functions_to_inline,
                                     unsigned &count) {
  functions_to_inline.clear();

  for (auto &I : instructions(F)) {
    if (auto *CI = dyn_cast<CallInst>(&I)) {
      Function *Callee = CI->getCalledFunction();
      if (Callee && !Callee->isDeclaration()) {
        functions_to_inline.insert(Callee);
        continue;
      }

      if (CI->isInlineAsm()) {
        if (!options_.auto_continue) {
          return AnalysisError::InlineAssembly;
        }
        continue;
      }
      if (!Callee) {
        if (!options_.auto_continue) {
          return AnalysisError::IndirectCall;
        }
        continue;
      }

      if (Callee->isIntrinsic()) {
        continue;
      }
      if (!options_.auto_continue &&
          !secure_function_names_.contains(Callee->getName())) {
        errs() << "No implementation for function: " << Callee->getName()
               << "\n";
        return AnalysisError::NoImplementation;
      }
    }
  }

  count = functions_to_inline.size();
  return AnalysisError::None;
}

AnalysisError
CryptoAnalysisImpl::inlineFunctionCalls(Function &F,
                                        std::set<Function *> &functions_to_inline,
                                        unsigned &count) {
  AnalysisError error = getFunctionCalls(F, functions_to_inline, count);
  if (isError(error)) {
    return error;
  }

  StringRef func_name = stripClonedSuffix(F.getName());

  for (Function *callee : functions_to_inline) {
    if (func_name == callee->getName()) {
      if (!options_.auto_continue) {
        return AnalysisError::InlineItself;
      }
      continue;
    }

    CallBase *CB = dyn_cast<CallBase>(callee->user_back());
    InlineFunctionInfo IFI;
    if (!CB) {
      if (!options_.auto_continue) {
        return AnalysisError::NotCallBase;
      }
      continue;
    }
    InlineResult IR = InlineFunction(*CB, IFI);
    if (!options_.auto_continue && !IR.isSuccess()) {
      return AnalysisError::InlineFail;
    }
  }

  return getFunctionCalls(F, functions_to_inline, count);
}

InliningResult CryptoAnalysisImpl::recursiveInlineCalls(Function *targetFunction) {
  ValueToValueMapTy VMap;
  Function *cloned_function = CloneFunction(targetFunction, VMap);
  cloned_function->setName((targetFunction->getName() + clonedFunctionSuffix()).str());

  unsigned inline_done = 1;
  int inline_counter = 0;
  while (inline_done != 0) {
    std::set<Function *> functions_to_inline;
    AnalysisError error =
        inlineFunctionCalls(*cloned_function, functions_to_inline, inline_done);
    if (isError(error)) {
      statistics_.cannot_inline_cases.push_back(error);
      cloned_function->eraseFromParent();
      return {nullptr, error};
    }
    inline_counter++;
    if (inline_counter > options_.inline_threshold) {
      statistics_.cannot_inline_cases.push_back(AnalysisError::OverThreshold);
      cloned_function->eraseFromParent();
      return {nullptr, AnalysisError::OverThreshold};
    }
  }

  assert(inline_done == 0 && "Inline function failed");
  return {cloned_function, AnalysisError::None};
}

} // namespace detail
} // namespace ctllvm
