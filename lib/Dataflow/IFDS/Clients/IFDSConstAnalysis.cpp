/*
 * IFDS Const Analysis Implementation
 */

#include "Dataflow/IFDS/Clients/IFDSConstAnalysis.h"

#include "Dataflow/IFDS/Utils/LLVMFlowHelpers.h"

#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Support/raw_ostream.h>

namespace ifds {

ConstAnalysis::ConstAnalysis() {}

ConstAnalysis::ConstAnalysis(lotus::AliasAnalysisWrapper *aa) {
  m_alias_analysis = aa;
}

void ConstAnalysis::set_alias_analysis(lotus::AliasAnalysisWrapper *aa) {
  m_alias_analysis = aa;
}

ConstFact ConstAnalysis::zero_fact() const { return ConstFact::zero(); }

ConstAnalysis::FactSet ConstAnalysis::normal_flow(const llvm::Instruction *stmt,
                                                  const llvm::Instruction *succ,
                                                  const ConstFact &fact) {
  FactSet result;

  if (fact.is_zero()) {
    result.insert(fact);
  }

  if (const auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(stmt)) {
    if (fact.is_zero()) {
      all_memory_locations.insert(alloca);
    }
    if (!fact.is_zero()) {
      result.insert(fact);
    }
  } else if (const auto *store = llvm::dyn_cast<llvm::StoreInst>(stmt)) {
    const llvm::Value *pointer_op = store->getPointerOperand();

    // Ignore vtable stores
    if (is_vtable_store(store)) {
      result.insert(fact);
      return result;
    }

    // Check if this location or any alias is already initialized
    bool already_initialized = false;
    if (fact.value == pointer_op || initialized_locations.count(pointer_op)) {
      already_initialized = true;
    }

    // Check aliases if we have alias analysis
    if (!already_initialized && m_alias_analysis) {
      for (const auto *loc : initialized_locations) {
        if (may_alias_or_equal(loc, pointer_op)) {
          already_initialized = true;
          break;
        }
      }
    }

    const bool fact_tracks_location =
        !fact.is_zero() && fact.value != nullptr &&
        may_alias_or_equal(fact.value, pointer_op);

    if (fact.is_zero() || fact_tracks_location) {
      if (already_initialized) {
        // Second write - mark as mutable
        result.insert(ConstFact::mutable_mem(pointer_op));
      } else {
        // First write - mark as initialized
        result.insert(ConstFact::initialized(pointer_op));
        mark_initialized(pointer_op);
      }
    }

    if (!fact.is_zero()) {
      result.insert(fact);
    }
  } else {
    result.insert(fact);
  }

  return result;
}

ConstAnalysis::FactSet ConstAnalysis::call_flow(const llvm::CallBase *call,
                                                const llvm::Function *callee,
                                                const ConstFact &fact) {
  FactSet result;

  if (fact.is_zero()) {
    result.insert(fact);
    return result;
  }

  flow::map_facts_to_callee(
      call, callee, fact, result,
      [](const llvm::Value *actual, const llvm::Argument *formal,
         const ConstFact &source) {
        return source.value == actual && formal->getType()->isPointerTy();
      },
      [](const llvm::Value * /*actual*/, const llvm::Argument *formal,
         const ConstFact &source) { return ConstFact(source.type, formal); });

  return result;
}

ConstAnalysis::FactSet ConstAnalysis::return_flow(
    const llvm::CallBase *call, const llvm::Instruction *exit_inst,
    const llvm::Instruction *return_site, const llvm::Function *callee,
    const ConstFact &exit_fact, const ConstFact & /*call_fact*/) {
  (void)exit_inst;
  FactSet result;

  if (exit_fact.is_zero()) {
    result.insert(exit_fact);
    return result;
  }

  flow::map_facts_to_caller(
      call, callee, exit_fact, result,
      [](const llvm::Argument *formal, const llvm::Value * /*actual*/,
         const ConstFact &source) {
        return source.value == formal && formal->getType()->isPointerTy();
      },
      [](const llvm::Argument * /*formal*/, const llvm::Value *actual,
         const ConstFact &source) { return ConstFact(source.type, actual); },
      [](const llvm::Value * /*ret_val*/, const ConstFact & /*source*/) {
        return false;
      },
      [](const llvm::Value * /*ret_val*/, const ConstFact &source) {
        return source;
      });

  return result;
}

ConstAnalysis::FactSet
ConstAnalysis::call_to_return_flow(const llvm::CallBase *call,
                                   const llvm::Instruction *return_site,
                                   llvm::ArrayRef<const llvm::Function *> callees, const ConstFact &fact) {
  FactSet result;

  // Handle memory intrinsics
  if (is_memory_intrinsic(call)) {
    if (fact.is_zero()) {
      // Check the first operand (destination) for memcpy/memmove/memset
      if (call->arg_size() > 0) {
        const llvm::Value *dest = call->getArgOperand(0);

        // Check if already initialized
        bool already_initialized = initialized_locations.count(dest) > 0;

        if (already_initialized) {
          result.insert(ConstFact::mutable_mem(dest));
        } else {
          result.insert(ConstFact::initialized(dest));
          mark_initialized(dest);
        }
      }
    }
    return result;
  }

  // For non-pointer facts, pass through; for pointer facts passed to callees,
  // kill them (callee may modify).
  flow::map_facts_alongside_callsite_with_policies(
      call, fact, result,
      [](const llvm::Value *arg, const ConstFact &source) {
        return arg == source.value && arg->getType()->isPointerTy();
      },
      [](const ConstFact &source) { return source.is_zero(); },
      [](const ConstFact &source) {
        return source.value && llvm::isa<llvm::GlobalValue>(source.value);
      },
      /*PropagateGlobals=*/true,
      /*PropagateZero=*/true);

  return result;
}

ConstAnalysis::FactSet
ConstAnalysis::initial_facts(const llvm::Function * /*main*/) {
  FactSet result;
  result.insert(zero_fact());
  return result;
}

bool ConstAnalysis::is_initialized(const llvm::Value *val) const {
  return initialized_locations.count(val) > 0;
}

void ConstAnalysis::mark_initialized(const llvm::Value *val) {
  initialized_locations.insert(val);
}

std::size_t ConstAnalysis::initialized_count() const {
  return initialized_locations.size();
}

bool ConstAnalysis::is_vtable_store(const llvm::StoreInst *store) const {
  // Check for vtable store pattern: store <vtable ptr>, <object ptr>
  const llvm::Value *value_op = store->getValueOperand();
  if (const auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(value_op)) {
    // Could be vtable related
    return false; // Conservative for now
  }
  return false;
}

bool ConstAnalysis::is_memory_intrinsic(const llvm::CallBase *call) const {
  if (!call->getCalledFunction()) {
    return false;
  }

  llvm::StringRef name = call->getCalledFunction()->getName();
  return name == "llvm.memcpy.p0i8.p0i8.i64" ||
         name == "llvm.memmove.p0i8.p0i8.i64" ||
         name == "llvm.memset.p0i8.i64" || name.startswith("llvm.memcpy") ||
         name.startswith("llvm.memmove") || name.startswith("llvm.memset");
}

std::set<const llvm::Value *> ConstAnalysis::get_context_relevant_aliases(
    const std::set<const llvm::Value *> &aliases,
    const llvm::Function *context) const {

  std::set<const llvm::Value *> relevant;
  for (const auto *alias : aliases) {
    // Keep if it's in the current function context
    if (const auto *inst = llvm::dyn_cast<llvm::Instruction>(alias)) {
      if (inst->getFunction() == context) {
        relevant.insert(alias);
        continue;
      }
    }
    // Keep allocas and globals
    if (llvm::isa<llvm::AllocaInst>(alias) ||
        llvm::isa<llvm::GlobalVariable>(alias)) {
      relevant.insert(alias);
    }
  }
  return relevant;
}

void ConstAnalysis::emit_report(llvm::raw_ostream &OS) const {
  OS << "========================================\n";
  OS << "Const Analysis Report\n";
  OS << "========================================\n";
  OS << "Total memory locations tracked: " << all_memory_locations.size()
     << "\n";
  OS << "Initialized locations: " << initialized_locations.size() << "\n";
  OS << "\nMutable memory locations:\n";

  for (const auto *loc : all_memory_locations) {
    if (initialized_locations.count(loc)) {
      OS << "  - ";
      loc->print(OS);
      OS << "\n";
    }
  }
}

} // namespace ifds
