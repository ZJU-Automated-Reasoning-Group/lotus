/*
 * IFDS Uninitialized Variables Analysis Implementation
 */

#include "Dataflow/IFDS/Clients/IFDSUninitializedVariables.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/Support/raw_ostream.h>

namespace ifds {

UninitializedVariablesAnalysis::UninitializedVariablesAnalysis() {}

UninitVarFact UninitializedVariablesAnalysis::zero_fact() const {
  return UninitVarFact::zero();
}

UninitializedVariablesAnalysis::FactSet
UninitializedVariablesAnalysis::normal_flow(const llvm::Instruction *stmt,
                                            const llvm::Instruction *succ,
                                            const UninitVarFact &fact) {
  FactSet result;

  if (fact.is_zero()) {
    result.insert(fact);
  }

  if (const auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(stmt)) {
    if (fact.is_zero()) {
      llvm::Type *allocated_type = alloca->getAllocatedType();
      if (allocated_type->isIntegerTy() ||
          allocated_type->isFloatingPointTy() ||
          allocated_type->isPointerTy() || allocated_type->isArrayTy()) {
        result.insert(UninitVarFact::uninitialized(alloca));
      }
    }
    if (!fact.is_zero()) {
      result.insert(fact);
    }
  } else if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(stmt)) {
    const llvm::Value *pointer_op = store->getPointerOperand();
    const llvm::Value *value_op = store->getValueOperand();

    if (fact.value == pointer_op) {
      // Initializing this location - kill the uninitialized fact
      // and generate an initialized fact
      result.insert(UninitVarFact::initialized(pointer_op));
    } else if (fact.value == value_op ||
               (fact.is_zero() && llvm::isa<llvm::UndefValue>(value_op))) {
      // Storing an uninitialized value - propagate to pointer
      result.insert(fact);
      result.insert(UninitVarFact::uninitialized(pointer_op));
    } else {
      result.insert(fact);
    }
  } else {
    // Check for uses of uninitialized values
    for (const auto &operand : stmt->operands()) {
      const llvm::Value *op_val = operand.get();
      if (op_val == fact.value && fact.is_uninitialized()) {
        // Check if this is a real use (not just a cast/gep/phi)
        if (!llvm::isa<llvm::GetElementPtrInst>(stmt) &&
            !llvm::isa<llvm::CastInst>(stmt) &&
            !llvm::isa<llvm::PHINode>(stmt)) {
          undef_uses[stmt].insert(fact.value);
        }
        // Propagate the uninitialized value through this instruction
        result.insert(UninitVarFact::uninitialized(stmt));
        break;
      }
    }

    if (result.empty()) {
      result.insert(fact);
    }
  }

  return result;
}

UninitializedVariablesAnalysis::FactSet
UninitializedVariablesAnalysis::call_flow(const llvm::CallBase *call,
                                          const llvm::Function *callee,
                                          const UninitVarFact &fact) {
  FactSet result;

  if (fact.is_zero()) {
    result.insert(fact);
    return result;
  }

  // Only map facts that correspond to actual arguments of this call into the
  // callee's formal parameters.  Unconditionally passing all caller-local
  // facts (the old "Also pass facts that are not parameters" line) was
  // unsound: it injected caller-local variables into the callee's scope,
  // causing spurious uninitialized-variable reports inside the callee for
  // values that have no meaning there.
  //
  // Global variables are an exception: they are visible in both caller and
  // callee, so we propagate them through unchanged.
  if (callee && !callee->isDeclaration()) {
    unsigned arg_idx = 0;
    for (const auto &arg : callee->args()) {
      if (arg_idx < call->arg_size()) {
        const llvm::Value *actual = call->getArgOperand(arg_idx);
        if (fact.value == actual) {
          // Map the caller's actual argument fact to the callee's formal.
          result.insert(UninitVarFact(fact.type, &arg));
        }
      }
      ++arg_idx;
    }
  }

  // Propagate global-variable facts: globals are shared between caller and
  // callee, so an uninitialized global must be visible inside the callee too.
  if (fact.value && llvm::isa<llvm::GlobalVariable>(fact.value)) {
    result.insert(fact);
  }

  return result;
}

UninitializedVariablesAnalysis::FactSet
UninitializedVariablesAnalysis::return_flow(
    const llvm::CallBase *call, const llvm::Instruction *exit_inst,
    const llvm::Instruction *return_site, const llvm::Function *callee,
    const UninitVarFact &exit_fact, const UninitVarFact & /*call_fact*/) {
  (void)exit_inst;
  FactSet result;

  if (exit_fact.is_zero()) {
    result.insert(exit_fact);
    return result;
  }

  // Map return value back to call site.
  // The old code checked exit_fact.value->getType()->isPointerTy() before
  // attempting the dyn_cast<ReturnInst>.  A ReturnInst is an Instruction, not
  // a pointer-typed value, so that type check always evaluated to false and
  // the return value was never mapped back.  The correct check is simply
  // whether exit_fact.value is a ReturnInst with a non-void return value.
  if (const auto *ret_inst =
          llvm::dyn_cast_or_null<llvm::ReturnInst>(exit_fact.value)) {
    if (ret_inst->getReturnValue()) {
      result.insert(UninitVarFact(exit_fact.type, call));
    }
  }

  // Map pointer parameters back to actuals
  if (callee && !callee->isDeclaration()) {
    unsigned arg_idx = 0;
    for (const auto &arg : callee->args()) {
      if (arg_idx < call->arg_size()) {
        const llvm::Value *actual = call->getArgOperand(arg_idx);
        if (exit_fact.value == &arg && arg.getType()->isPointerTy()) {
          result.insert(UninitVarFact(exit_fact.type, actual));
        }
      }
      ++arg_idx;
    }
  }

  return result;
}

UninitializedVariablesAnalysis::FactSet
UninitializedVariablesAnalysis::call_to_return_flow(const llvm::CallBase *call,
                                                    const llvm::Instruction *return_site,
                                                    llvm::ArrayRef<const llvm::Function *> callees, const UninitVarFact &fact) {
  FactSet result;

  // For most facts, pass them through
  // Kill facts that are passed by pointer and might be modified by callee
  bool is_pointer_param = false;
  for (unsigned i = 0; i < call->arg_size(); ++i) {
    if (call->getArgOperand(i) == fact.value &&
        call->getArgOperand(i)->getType()->isPointerTy()) {
      is_pointer_param = true;
      break;
    }
  }

  if (!is_pointer_param) {
    result.insert(fact);
  }

  // Check for sources at the call site.
  // malloc/alloca return uninitialized memory.
  // calloc zero-initializes its allocation, so it must NOT be treated as
  // uninitialized — the old code incorrectly included it here.
  if (fact.is_zero() && call->getCalledFunction()) {
    llvm::StringRef name = call->getCalledFunction()->getName();
    if (name == "malloc" || name == "alloca") {
      result.insert(UninitVarFact::uninitialized(call));
    }
    // calloc is intentionally excluded: it guarantees zero-initialization.
  }

  return result;
}

UninitializedVariablesAnalysis::FactSet
UninitializedVariablesAnalysis::initial_facts(const llvm::Function *main) {
  FactSet result;
  result.insert(zero_fact());

  // Function arguments are considered initialized (they have values)
  for (const auto &arg : main->args()) {
    result.insert(UninitVarFact::initialized(&arg));
  }

  return result;
}

bool UninitializedVariablesAnalysis::is_source(
    const llvm::Instruction *inst) const {
  auto it = undef_uses.find(inst);
  return it != undef_uses.end() && !it->second.empty();
}

std::vector<UninitializedVariablesAnalysis::UninitResult>
UninitializedVariablesAnalysis::get_results() const {
  std::vector<UninitResult> results;

  for (const auto &pair : undef_uses) {
    for (const auto *val : pair.second) {
      results.emplace_back(pair.first, val);
    }
  }

  return results;
}

void UninitializedVariablesAnalysis::emit_report(llvm::raw_ostream &OS) const {
  auto results = get_results();

  OS << "========================================\n";
  OS << "Uninitialized Variables Analysis Report\n";
  OS << "========================================\n";
  OS << "Found " << results.size() << " use(s) of uninitialized variables:\n\n";

  for (const auto &result : results) {
    OS << "Use at: ";
    result.use_site->print(OS);
    OS << "\n";

    if (result.uninitialized_value) {
      OS << "  Uninitialized value: ";
      result.uninitialized_value->print(OS);
      OS << "\n";
    }
    OS << "\n";
  }
}

bool UninitializedVariablesAnalysis::is_initialized(
    const llvm::Value *val) const {
  return initialized_locations.count(val) > 0;
}

void UninitializedVariablesAnalysis::mark_initialized(const llvm::Value *val) {
  initialized_locations.insert(val);
}

bool UninitializedVariablesAnalysis::may_be_uninitialized(
    const llvm::Value *val, const FactSet &facts) const {
  for (const auto &fact : facts) {
    if (fact.value == val && fact.is_uninitialized()) {
      return true;
    }
  }
  return false;
}

} // namespace ifds
