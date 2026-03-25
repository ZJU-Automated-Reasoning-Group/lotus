#include "Dataflow/IFDS/Clients/IFDSTypeAnalysis.h"

#include <llvm/IR/Instructions.h>

namespace ifds {

TypeAnalysis::FactSet TypeAnalysis::normal_flow(const llvm::Instruction *stmt,
                                                const llvm::Instruction *succ,
                                                const Fact &fact) {
  FactSet result;
  if (!stmt) {
    return result;
  }

  // Preserve existing fact by default.
  result.insert(fact);

  // Generate newly defined SSA values from related input facts.
  if (!stmt->getType()->isVoidTy()) {
    const Fact def = stmt;

    if (fact == zero_fact()) {
      result.insert(def);
      return result;
    }

    for (const auto &operand : stmt->operands()) {
      if (operand.get() == fact) {
        result.insert(def);
        break;
      }
    }
  }

  return result;
}

TypeAnalysis::FactSet TypeAnalysis::call_flow(const llvm::CallBase *call,
                                              const llvm::Function *callee,
                                              const Fact &fact) {
  FactSet result;
  if (!call) {
    return result;
  }

  if (fact == zero_fact()) {
    result.insert(fact);
  }

  if (!callee || callee->isDeclaration()) {
    return result;
  }

  unsigned idx = 0;
  for (const llvm::Argument &formal : callee->args()) {
    if (idx >= call->arg_size()) {
      break;
    }
    if (fact == call->getArgOperand(idx)) {
      result.insert(&formal);
    }
    ++idx;
  }
  return result;
}

TypeAnalysis::FactSet TypeAnalysis::return_flow(const llvm::CallBase *call,
                                                const llvm::Instruction *exit_inst,
                                                const llvm::Instruction *return_site, const llvm::Function *callee,
                                                const Fact &exit_fact,
                                                const Fact &call_fact) {
  FactSet result;
  if (!call) {
    return result;
  }

  if (exit_fact == zero_fact()) {
    result.insert(exit_fact);
    return result;
  }

  if (call_fact != zero_fact()) {
    result.insert(call_fact);
  }

  if (!callee || callee->isDeclaration()) {
    return result;
  }

  // Formal-to-actual mapping for pointer/reference-style values.
  unsigned idx = 0;
  for (const llvm::Argument &formal : callee->args()) {
    if (idx >= call->arg_size()) {
      break;
    }
    if (exit_fact == &formal) {
      result.insert(call->getArgOperand(idx));
    }
    ++idx;
  }

  // Return-value mapping to call-site SSA result.
  if (!call->getType()->isVoidTy()) {
    for (const llvm::BasicBlock &bb : *callee) {
      if (const auto *ret =
              llvm::dyn_cast<llvm::ReturnInst>(bb.getTerminator())) {
        if (ret->getReturnValue() == exit_fact) {
          result.insert(call);
          break;
        }
      }
    }
  }

  return result;
}

TypeAnalysis::FactSet
TypeAnalysis::call_to_return_flow(const llvm::CallBase * /*call*/,
                                  const llvm::Instruction *return_site,
                                  llvm::ArrayRef<const llvm::Function *>
                                      callees,
                                  const Fact &fact) {
  (void)return_site;
  (void)callees;
  FactSet result;
  result.insert(fact);
  return result;
}

TypeAnalysis::FactSet TypeAnalysis::initial_facts(const llvm::Function *main) {
  FactSet result;
  result.insert(zero_fact());
  if (!main) {
    return result;
  }
  for (const llvm::Argument &arg : main->args()) {
    result.insert(&arg);
  }
  return result;
}

std::map<const llvm::Value *, const llvm::Type *> TypeAnalysis::infer_types(
    const std::unordered_map<const llvm::Instruction *, FactSet> &facts) const {
  std::map<const llvm::Value *, const llvm::Type *> types;
  for (const auto &entry : facts) {
    for (const Fact &fact : entry.second) {
      if (fact == zero_fact()) {
        continue;
      }
      types[fact] = fact->getType();
    }
  }
  return types;
}

} // namespace ifds
