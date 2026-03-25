/*
 * LLVM IFDS flow-function helpers (Lotus counterpart to Phasar LLVM helpers)
 */

#pragma once

#include <algorithm>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>

namespace ifds {
namespace flow {

template <typename FactSet, typename Fact, typename ShouldPropagate,
          typename MakeFact>
void map_facts_to_callee(const llvm::CallBase *Call,
                         const llvm::Function *Callee, const Fact &Source,
                         FactSet &Out, ShouldPropagate &&ShouldPropagateArg,
                         MakeFact &&MakeMappedFact) {
  if (!Call || !Callee || Callee->isDeclaration()) {
    return;
  }

  const unsigned NumArgs =
      std::min(Call->arg_size(), static_cast<unsigned>(std::distance(
                                     Callee->arg_begin(), Callee->arg_end())));

  for (unsigned I = 0; I < NumArgs; ++I) {
    const llvm::Value *Actual = Call->getArgOperand(I);
    if (!Actual) {
      continue;
    }
    const auto *ParamIt = Callee->arg_begin();
    std::advance(ParamIt, I);
    if (ParamIt == Callee->arg_end()) {
      break;
    }
    const llvm::Argument *Formal = &*ParamIt;

    if (ShouldPropagateArg(Actual, Formal, Source)) {
      Out.insert(MakeMappedFact(Actual, Formal, Source));
    }
  }
}

template <typename FactSet, typename Fact, typename ShouldPropagateParam,
          typename MakeMappedParamFact, typename ShouldPropagateRet,
          typename MakeMappedRetFact>
void map_facts_to_caller(const llvm::CallBase *Call,
                         const llvm::Function *Callee, const Fact &Source,
                         FactSet &Out,
                         ShouldPropagateParam &&ShouldPropagateParameter,
                         MakeMappedParamFact &&MakeParamFact,
                         ShouldPropagateRet &&ShouldPropagateReturn,
                         MakeMappedRetFact &&MakeReturnFact) {
  if (!Call || !Callee) {
    return;
  }

  if (!Callee->isDeclaration()) {
    const unsigned NumArgs =
        std::min(Call->arg_size(),
                 static_cast<unsigned>(
                     std::distance(Callee->arg_begin(), Callee->arg_end())));
    for (unsigned I = 0; I < NumArgs; ++I) {
      const llvm::Value *Actual = Call->getArgOperand(I);
      if (!Actual) {
        continue;
      }
      const auto *ParamIt = Callee->arg_begin();
      std::advance(ParamIt, I);
      if (ParamIt == Callee->arg_end()) {
        break;
      }
      const llvm::Argument *Formal = &*ParamIt;
      if (ShouldPropagateParameter(Formal, Actual, Source)) {
        Out.insert(MakeParamFact(Formal, Actual, Source));
      }
    }

    for (const llvm::BasicBlock &BB : *Callee) {
      for (const llvm::Instruction &Inst : BB) {
        const auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(&Inst);
        if (!Ret) {
          continue;
        }
        const llvm::Value *RetVal = Ret->getReturnValue();
        if (ShouldPropagateReturn(RetVal, Source)) {
          Out.insert(MakeReturnFact(RetVal, Source));
        }
      }
    }
  }
}

template <typename FactSet, typename Fact, typename ShouldKill>
void map_facts_alongside_callsite(const llvm::CallBase *Call,
                                  const Fact &Source, FactSet &Out,
                                  ShouldKill &&ShouldKillArg) {
  if (!Call) {
    return;
  }
  bool Killed = false;
  for (unsigned I = 0; I < Call->arg_size(); ++I) {
    const llvm::Value *Arg = Call->getArgOperand(I);
    if (Arg && ShouldKillArg(Arg, Source)) {
      Killed = true;
      break;
    }
  }
  if (!Killed) {
    Out.insert(Source);
  }
}

template <typename FactSet, typename Fact, typename ShouldKill, typename IsZero,
          typename IsGlobal>
void map_facts_alongside_callsite_with_policies(
    const llvm::CallBase *Call, const Fact &Source, FactSet &Out,
    ShouldKill &&ShouldKillArg, IsZero &&IsZeroFact, IsGlobal &&IsGlobalFact,
    bool PropagateGlobals = true, bool PropagateZero = true) {
  if (!Call) {
    return;
  }

  if (IsZeroFact(Source)) {
    if (PropagateZero) {
      Out.insert(Source);
    }
    return;
  }

  if (IsGlobalFact(Source)) {
    if (PropagateGlobals) {
      Out.insert(Source);
    }
    return;
  }

  bool Killed = false;
  for (unsigned I = 0; I < Call->arg_size(); ++I) {
    const llvm::Value *Arg = Call->getArgOperand(I);
    if (Arg && ShouldKillArg(Arg, Source)) {
      Killed = true;
      break;
    }
  }
  if (!Killed) {
    Out.insert(Source);
  }
}

} // namespace flow
} // namespace ifds
