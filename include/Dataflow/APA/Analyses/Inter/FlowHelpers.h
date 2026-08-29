#ifndef DATAFLOW_APA_ANALYSES_INTER_FLOWHELPERS_H_
#define DATAFLOW_APA_ANALYSES_INTER_FLOWHELPERS_H_

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"

#include <type_traits>
#include <utility>

namespace elimination {
namespace llvm_inter {

template <typename CallbackT>
void forEachActualFormalPair(const llvm::CallBase *Call, llvm::Function *Callee,
                             CallbackT &&Callback) {
  if (Call == nullptr || Callee == nullptr) {
    return;
  }

  unsigned Index = 0;
  for (auto &Arg : Callee->args()) {
    if (Index >= Call->arg_size()) {
      break;
    }
    Callback(Call->getArgOperand(Index), &Arg, Index);
    ++Index;
  }
}

template <typename FactT, typename PredicateT>
void copyFactsIf(const FactT &In, FactT &Out, PredicateT &&Predicate) {
  for (const auto &Entry : In) {
    if (Predicate(Entry)) {
      Out.insert(Entry);
    }
  }
}

template <typename FactT> void copyGlobalValueFacts(const FactT &In, FactT &Out) {
  copyFactsIf(In, Out, [](const auto &Entry) {
    if constexpr (std::is_pointer_v<std::decay_t<decltype(Entry)>>) {
      return llvm::isa<llvm::GlobalValue>(Entry);
    } else {
      return Entry.first != nullptr && llvm::isa<llvm::GlobalValue>(Entry.first);
    }
  });
}

inline bool hasConcreteReturnValue(const llvm::CallBase *Call,
                                   const llvm::ReturnInst *Ret) {
  return Call != nullptr && Ret != nullptr && !Call->getType()->isVoidTy() &&
         Ret->getReturnValue() != nullptr;
}

template <typename FactT> void copyStoreFacts(const FactT &In, FactT &Out) {
  copyFactsIf(In, Out, [](const auto *Entry) { return llvm::isa<llvm::StoreInst>(Entry); });
}

} // namespace llvm_inter
} // namespace elimination

#endif // DATAFLOW_APA_ANALYSES_INTER_FLOWHELPERS_H_
