#include "Checker/Pulse/Interproc/PulseSpecialization.h"

#include "Checker/Pulse/Core/PulseFormula.h"
#include "Checker/Pulse/Domain/PulseAbductiveDomain.h"
#include "Checker/Pulse/Domain/PulseOperations.h"

#include <algorithm>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>

namespace pulse {

SpecializationKey SpecializationManager::computeSpecializationKey(
    const AbductiveDomain &caller_state, const llvm::CallInst *call,
    const std::vector<AbstractValue> &actual_args) {
  SpecializationKey key;

  // Extract dynamic types and heap paths from actual arguments
  for (size_t i = 0; i < actual_args.size() && i < call->arg_size(); ++i) {
    AbstractValue arg = actual_args[i];
    AbstractValue canon = caller_state.getCanonical(arg);

    // Build heap path from argument
    // Start with Pvar element (representing the argument)
    HeapPath path;
    path.push(HeapPath::Element(HeapPath::PathElement::Pvar));

    // Check if argument has heap edges that need specialization
    const auto &edges = caller_state.getPostHeap().getEdges();
    auto edge_it = edges.find(canon);
    if (edge_it != edges.end()) {
      // This argument has heap structure - track it for specialization
      key.heap_paths_to_values[path] = canon;

      // Also check for dynamic type information in attributes
      // If we have type information, add to dynamic_types
      // (Simplified: would need to extract actual type from LLVM)
      const llvm::Value *arg_val = call->getArgOperand(i);
      if (arg_val && arg_val->getType()->isPointerTy()) {
        // Check if we can determine a more specific type
        // For now, we just track that this path might need specialization
      }
    }

    // Check for dynamic type in path formula
    // If formula has type constraints, extract them
    const auto &formula = caller_state.getPathFormula();
    // (Full implementation would query formula for type constraints)
  }

  // Detect aliasing: if multiple arguments point to same abstract value
  for (size_t i = 0; i < actual_args.size(); ++i) {
    AbstractValue arg_i = caller_state.getCanonical(actual_args[i]);
    for (size_t j = i + 1; j < actual_args.size(); ++j) {
      AbstractValue arg_j = caller_state.getCanonical(actual_args[j]);
      if (arg_i == arg_j) {
        // Arguments are aliased
        key.aliasing_map[arg_i].insert(arg_j);
        key.aliasing_map[arg_j].insert(arg_i);
      }
    }
  }

  return key;
}

AbductiveDomain
SpecializationManager::applySpecialization(const AbductiveDomain &astate,
                                           const SpecializationKey &key) {
  AbductiveDomain specialized = astate.clone();

  // Apply aliasing constraints: if aliases are specified, add equalities
  for (const auto &kv : key.aliasing_map) {
    AbstractValue v1 = kv.first;
    for (AbstractValue v2 : kv.second) {
      // Add equality constraint: v1 == v2
      specialized.getPathFormula().addEquality(v1, v2);
    }
  }

  // Apply dynamic type constraints
  for (const auto &kv : key.dynamic_types) {
    AbstractValue av = kv.first;
    const std::string &type_name = kv.second;

    // In a full implementation, we would:
    // 1. Parse type_name to get LLVM Type
    // 2. Add type constraint to formula using
    // PulseArithmetic.and_dynamic_type_is For now, we track it in
    // need_dynamic_type_specialization
    specialized.addNeedDynamicTypeSpecialization(av);

    // Note: Full implementation would call something like:
    // specialized.getPathFormula().addDynamicType(av, type_name);
  }

  // Apply heap path constraints
  // For each heap path that needs specialization, we would:
  // 1. Initialize the heap path in the domain
  // 2. Add constraints based on the path
  for (const auto &kv : key.heap_paths_to_values) {
    const HeapPath &path = kv.first;
    AbstractValue value = kv.second;

    // In a full implementation, we would:
    // 1. Follow the heap path to get the address
    // 2. Add constraints based on what we find
    // For now, we just track that this path needs attention
    (void)path;
    (void)value;

    // Example: if path is p->field, we would:
    // - Get address of p->field
    // - Add type constraints if needed
    // - Add aliasing constraints if needed
  }

  return specialized;
}

/**
 * Helper: initialize heap path in domain (similar to Infer's
 * initialize_heap_path)
 */
static std::pair<AbductiveDomain, std::pair<AbstractValue, ValueHistory>>
initializeHeapPath(const HeapPath &heap_path, const AbductiveDomain &astate) {
  // Simplified implementation
  // Full implementation would:
  // 1. Start from Pvar and get its address
  // 2. Follow FieldAccess/Dereference/ArrayIndex elements
  // 3. Return the final address and history

  AbductiveDomain result = astate.clone();
  AbstractValue addr; // Would be computed from path
  ValueHistory hist;

  // For now, return a placeholder
  std::pair<AbstractValue, ValueHistory> addr_hist = std::make_pair(addr, hist);
  return std::make_pair(std::move(result), std::move(addr_hist));
}

/**
 * Helper: prune equalities for a list of values (similar to Infer's
 * prune_eq_list_values)
 */
static AbductiveDomain
pruneEqListValues(const std::vector<AbstractValue> &values,
                  const AbductiveDomain &astate) {
  if (values.empty()) {
    return astate.clone();
  }

  AbductiveDomain result = astate.clone();
  AbstractValue head = values[0];

  for (size_t i = 1; i < values.size(); ++i) {
    // Add equality: head == values[i]
    result.getPathFormula().addEquality(head, values[i]);
  }

  return result;
}

} // namespace pulse
