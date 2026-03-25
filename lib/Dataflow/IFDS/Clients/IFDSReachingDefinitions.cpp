/*
 * Reaching Definitions Analysis Implementation
 *
 * Author: rainoftime
 */

#include "Dataflow/IFDS/Clients/IFDSReachingDefinitions.h"

#include "Dataflow/IFDS/Utils/LLVMFlowHelpers.h"

#include <iostream>

#include <llvm/Support/raw_ostream.h>

namespace ifds {

// ============================================================================
// DefinitionFact Implementation
// ============================================================================

DefinitionFact::DefinitionFact()
    : m_type(ZERO), m_variable(nullptr), m_definition_site(nullptr) {}

DefinitionFact DefinitionFact::zero() { return DefinitionFact(); }

DefinitionFact DefinitionFact::definition(const llvm::Value *var,
                                          const llvm::Instruction *def_site) {
  DefinitionFact fact;
  fact.m_type = DEFINITION;
  fact.m_variable = var;
  fact.m_definition_site = def_site;
  return fact;
}

bool DefinitionFact::operator==(const DefinitionFact &other) const {
  if (m_type != other.m_type)
    return false;
  if (m_type == ZERO)
    return true;
  return m_variable == other.m_variable &&
         m_definition_site == other.m_definition_site;
}

bool DefinitionFact::operator<(const DefinitionFact &other) const {
  if (m_type != other.m_type)
    return m_type < other.m_type;
  if (m_type == ZERO)
    return false;
  if (m_variable != other.m_variable)
    return m_variable < other.m_variable;
  return m_definition_site < other.m_definition_site;
}

bool DefinitionFact::operator!=(const DefinitionFact &other) const {
  return !(*this == other);
}

DefinitionFact::Type DefinitionFact::get_type() const { return m_type; }

const llvm::Value *DefinitionFact::get_variable() const { return m_variable; }

const llvm::Instruction *DefinitionFact::get_definition_site() const {
  return m_definition_site;
}

bool DefinitionFact::is_zero() const { return m_type == ZERO; }

bool DefinitionFact::is_definition() const { return m_type == DEFINITION; }

std::ostream &operator<<(std::ostream &os, const DefinitionFact &fact) {
  if (fact.is_zero()) {
    os << "⊥";
  } else {
    os << "Def(" << fact.m_variable->getName().str() << " @ ";
    if (fact.m_definition_site->getParent()) {
      os << fact.m_definition_site->getParent()->getName().str() << ")";
    } else {
      os << "?)";
    }
  }
  return os;
}

// ============================================================================
// ReachingDefinitionsAnalysis Implementation
// ============================================================================

DefinitionFact ReachingDefinitionsAnalysis::zero_fact() const {
  return DefinitionFact::zero();
}

ReachingDefinitionsAnalysis::FactSet
ReachingDefinitionsAnalysis::normal_flow(const llvm::Instruction *stmt,
                                         const llvm::Instruction *succ,
                                         const DefinitionFact &fact) {
  FactSet result;

  // Always propagate zero fact
  if (fact.is_zero()) {
    result.insert(fact);
  }

  // Check if this instruction defines a variable
  if (defines_variable(stmt)) {
    const llvm::Value *defined_var = get_defined_variable(stmt);

    // Kill existing definitions of the same variable
    if (fact.is_definition() && fact.get_variable() == defined_var) {
      // This fact is killed by the new definition — do NOT propagate it.
    } else {
      // Propagate existing facts that are not killed
      if (!fact.is_zero()) {
        result.insert(fact);
      }
    }

    // Generate new definition fact.
    // BUG (fixed): the old code only generated the new definition when
    // fact.is_zero(), which means definitions were only created once (from
    // the zero fact) and never re-generated when an existing definition
    // fact flowed through the same instruction.  In standard reaching-
    // definitions analysis, a new definition is generated for EVERY
    // incoming fact (the zero fact acts as the "always-on" generator, but
    // non-zero facts also need to see the new definition downstream).
    // We generate the new definition unconditionally so that all paths
    // through this instruction carry the new definition.
    result.insert(DefinitionFact::definition(defined_var, stmt));

  } else {
    // No definition - just propagate existing facts
    if (!fact.is_zero()) {
      result.insert(fact);
    }
  }

  return result;
}

ReachingDefinitionsAnalysis::FactSet
ReachingDefinitionsAnalysis::call_flow(const llvm::CallBase *call,
                                       const llvm::Function *callee,
                                       const DefinitionFact &fact) {
  FactSet result;

  // Always propagate zero fact
  if (fact.is_zero()) {
    result.insert(fact);
  }

  if (!callee || callee->isDeclaration()) {
    return result;
  }

  if (fact.is_definition()) {
    flow::map_facts_to_callee(
        call, callee, fact, result,
        [this](const llvm::Value *actual, const llvm::Argument * /*formal*/,
               const DefinitionFact &source) {
          if (source.get_variable() == actual) {
            return true;
          }
          return source.get_variable() && actual &&
                 source.get_variable()->getType()->isPointerTy() &&
                 actual->getType()->isPointerTy() &&
                 may_alias_or_equal(source.get_variable(), actual);
        },
        [callee](const llvm::Value * /*actual*/, const llvm::Argument *formal,
                 const DefinitionFact & /*source*/) {
          const llvm::Instruction *entry_inst =
              &callee->getEntryBlock().front();
          return DefinitionFact::definition(formal, entry_inst);
        });
  }

  return result;
}

ReachingDefinitionsAnalysis::FactSet ReachingDefinitionsAnalysis::return_flow(
    const llvm::CallBase *call, const llvm::Instruction *exit_inst,
    const llvm::Instruction *return_site, const llvm::Function *callee,
    const DefinitionFact &exit_fact, const DefinitionFact &call_fact) {
  (void)exit_inst;
  FactSet result;

  // Always propagate zero fact
  if (exit_fact.is_zero()) {
    result.insert(exit_fact);
  }

  if (exit_fact.is_definition()) {
    flow::map_facts_to_caller(
        call, callee, exit_fact, result,
        [](const llvm::Argument * /*formal*/, const llvm::Value * /*actual*/,
           const DefinitionFact & /*source*/) { return false; },
        [](const llvm::Argument * /*formal*/, const llvm::Value * /*actual*/,
           const DefinitionFact &source) { return source; },
        [](const llvm::Value *ret_val, const DefinitionFact &source) {
          return ret_val == source.get_variable();
        },
        [call](const llvm::Value * /*ret_val*/, const DefinitionFact &source) {
          return DefinitionFact::definition(call, source.get_definition_site());
        });
  }

  // Propagate call site facts that represent local variables
  if (call_fact.is_definition() && is_local_to_caller(call_fact, callee)) {
    result.insert(call_fact);
  }

  return result;
}

ReachingDefinitionsAnalysis::FactSet
ReachingDefinitionsAnalysis::call_to_return_flow(const llvm::CallBase *call,
                                                 const llvm::Instruction *return_site,
                                                 llvm::ArrayRef<const llvm::Function *> callees, const DefinitionFact &fact) {
  FactSet result;

  const llvm::Function *callee = call->getCalledFunction();

  // For external functions, model their effects
  if (!callee || callee->isDeclaration()) {
    // External function call
    std::string func_name = callee ? callee->getName().str() : "";

    // Handle functions that might modify global state or return values
    if (func_name == "malloc" || func_name == "calloc") {
      // Memory allocation - creates a new definition
      if (fact.is_zero()) {
        result.insert(DefinitionFact::definition(call, call));
      }
    }

    flow::map_facts_alongside_callsite_with_policies(
        call, fact, result,
        [this, call](const llvm::Value * /*arg*/,
                     const DefinitionFact &source) {
          return source.is_definition() &&
                 is_killed_by_external_call(source, call);
        },
        [](const DefinitionFact &source) { return source.is_zero(); },
        [](const DefinitionFact &source) {
          return source.is_definition() &&
                 llvm::isa<llvm::GlobalValue>(source.get_variable());
        },
        /*PropagateGlobals=*/false,
        /*PropagateZero=*/true);
  } else {
    flow::map_facts_alongside_callsite_with_policies(
        call, fact, result,
        [this, callee](const llvm::Value * /*arg*/,
                       const DefinitionFact &source) {
          return source.is_definition() && !is_local_to_caller(source, callee);
        },
        [](const DefinitionFact &source) { return source.is_zero(); },
        [](const DefinitionFact &source) {
          return source.is_definition() &&
                 llvm::isa<llvm::GlobalValue>(source.get_variable());
        },
        /*PropagateGlobals=*/false,
        /*PropagateZero=*/true);
  }

  return result;
}

ReachingDefinitionsAnalysis::FactSet
ReachingDefinitionsAnalysis::initial_facts(const llvm::Function *main) {
  FactSet result;
  result.insert(zero_fact());

  // Add initial definitions for function parameters
  for (const llvm::Argument &arg : main->args()) {
    const llvm::Instruction *entry_inst = &main->getEntryBlock().front();
    result.insert(DefinitionFact::definition(&arg, entry_inst));
  }

  return result;
}

std::vector<const llvm::Instruction *>
ReachingDefinitionsAnalysis::get_reaching_definitions(
    const llvm::Instruction *use_site, const llvm::Value *variable) const {

  std::vector<const llvm::Instruction *> definitions;

  // This would query the analysis results
  // Implementation depends on how results are stored

  return definitions;
}

bool ReachingDefinitionsAnalysis::defines_variable(
    const llvm::Instruction *inst) const {
  // Check if instruction defines a variable
  return !inst->getType()->isVoidTy() || llvm::isa<llvm::StoreInst>(inst) ||
         llvm::isa<llvm::AllocaInst>(inst);
}

const llvm::Value *ReachingDefinitionsAnalysis::get_defined_variable(
    const llvm::Instruction *inst) const {
  if (auto *store = llvm::dyn_cast<llvm::StoreInst>(inst)) {
    return store->getPointerOperand();
  } else if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(inst)) {
    return alloca;
  } else if (!inst->getType()->isVoidTy()) {
    return inst;
  }
  return nullptr;
}

bool ReachingDefinitionsAnalysis::is_local_to_caller(
    const DefinitionFact &fact, const llvm::Function *callee) const {
  if (!fact.is_definition())
    return false;

  const llvm::Value *var = fact.get_variable();

  // Check if the variable is local to the caller (not a parameter or global)
  if (llvm::isa<llvm::GlobalValue>(var)) {
    return false; // Global variable
  }

  if (llvm::isa<llvm::Argument>(var)) {
    // Check if it's a parameter of the callee
    for (const llvm::Argument &arg : callee->args()) {
      if (&arg == var) {
        return false; // Parameter of callee
      }
    }
  }

  return true; // Local to caller
}

bool ReachingDefinitionsAnalysis::is_killed_by_external_call(
    const DefinitionFact &fact, const llvm::CallBase *call) const {
  if (!fact.is_definition())
    return false;

  const llvm::Value *var = fact.get_variable();

  // Conservative: external calls might modify global variables and
  // memory locations passed as pointer arguments
  if (llvm::isa<llvm::GlobalValue>(var)) {
    return true; // Assume external calls can modify globals
  }

  // Check if the variable is passed as a pointer argument
  for (unsigned i = 0; i < call->getNumOperands() - 1; ++i) {
    const llvm::Value *arg = call->getOperand(i);
    if (arg->getType()->isPointerTy() && may_alias_or_equal(arg, var)) {
      return true; // Might be modified through pointer
    }
  }

  return false;
}

} // namespace ifds

// Hash function implementation
namespace std {
size_t
hash<ifds::DefinitionFact>::operator()(const ifds::DefinitionFact &fact) const {
  if (fact.is_zero())
    return 0;
  // Use FNV-1a-style mixing to avoid the collision problems of XOR-shifting
  // aligned pointer hashes by 1 bit.
  size_t h = 14695981039346656037ULL;
  h ^= std::hash<const llvm::Value *>{}(fact.get_variable());
  h *= 1099511628211ULL;
  h ^= std::hash<const llvm::Instruction *>{}(fact.get_definition_site());
  h *= 1099511628211ULL;
  return h;
}
} // namespace std
