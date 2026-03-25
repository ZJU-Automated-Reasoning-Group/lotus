#ifndef CHECKER_PULSE_PULSECONTRADICTION_H
#define CHECKER_PULSE_PULSECONTRADICTION_H

#include "Checker/Pulse/Core/PulseAbstractValue.h"
#include "Checker/Pulse/Interproc/PulseSpecialization.h"

#include <map>
#include <set>
#include <string>
#include <vector>

#include <llvm/ADT/Optional.h>

namespace llvm {
class Value;
class Instruction;
} // namespace llvm

namespace pulse {

class PulseFormula;
class CallState;

/**
 * Contradiction types when applying summaries, following Infer's design.
 */
enum class ContradictionKind {
  None,
  Aliasing,                   // Distinct formals in pre are aliased in caller
  AliasingWithAllAliases,     // Aliasing with collected alias classes
  PathCondition,              // Path condition is UNSAT
  FormalActualLength,         // Mismatch in formal/actual argument counts
  CapturedFormalActualLength, // Mismatch in captured formal/actual counts (for
                              // closures)
  DynamicTypeNeeded           // Dynamic type specialization needed
};

// Forward declaration
class CallState;

/**
 * Contradiction information for summary application.
 * Enhanced to align with Infer's contradiction types.
 */
struct Contradiction {
  ContradictionKind kind;

  // For Aliasing contradiction - includes call_state for context
  struct {
    AbstractValue addr_caller;
    AbstractValue addr_callee;
    AbstractValue addr_callee_prime;
    const CallState *call_state; // Context during summary application
  } aliasing;

  // For AliasingWithAllAliases - uses heap paths
  std::vector<std::vector<HeapPath>> alias_classes_with_paths;

  // For PathCondition - includes UNSAT information
  struct {
    std::string unsat_reason;
    // Optional: store the conflicting constraints
    std::vector<std::string> conflicting_constraints;
  } path_condition;

  // For FormalActualLength
  struct {
    unsigned formal_count;
    unsigned actual_count;
    // Store which formals/actuals for better diagnostics
    std::vector<AbstractValue> formals;
    std::vector<AbstractValue> actuals;
  } length_mismatch;

  // For CapturedFormalActualLength
  struct {
    unsigned captured_formal_count;
    unsigned captured_actual_count;
    std::vector<AbstractValue> captured_formals;
    std::vector<AbstractValue> captured_actuals;
  } captured_length_mismatch;

  // For DynamicTypeNeeded - maps heap paths to abstract values
  std::map<HeapPath, AbstractValue> dynamic_type_paths;

  Contradiction()
      : kind(ContradictionKind::None),
        aliasing{AbstractValue(), AbstractValue(), AbstractValue(), nullptr} {}

  static Contradiction makeAliasing(AbstractValue caller, AbstractValue callee1,
                                    AbstractValue callee2,
                                    const CallState *call_state = nullptr) {
    Contradiction c;
    c.kind = ContradictionKind::Aliasing;
    c.aliasing.addr_caller = caller;
    c.aliasing.addr_callee = callee1;
    c.aliasing.addr_callee_prime = callee2;
    c.aliasing.call_state = call_state;
    return c;
  }

  static Contradiction
  makePathCondition(const std::string &reason,
                    const std::vector<std::string> &conflicting = {}) {
    Contradiction c;
    c.kind = ContradictionKind::PathCondition;
    c.path_condition.unsat_reason = reason;
    c.path_condition.conflicting_constraints = conflicting;
    return c;
  }

  static Contradiction
  makeCapturedFormalActualLength(unsigned captured_formal_count,
                                 unsigned captured_actual_count) {
    Contradiction c;
    c.kind = ContradictionKind::CapturedFormalActualLength;
    c.captured_length_mismatch.captured_formal_count = captured_formal_count;
    c.captured_length_mismatch.captured_actual_count = captured_actual_count;
    return c;
  }

  static Contradiction makeAliasingWithAllAliases(
      const std::vector<std::vector<HeapPath>> &alias_classes) {
    Contradiction c;
    c.kind = ContradictionKind::AliasingWithAllAliases;
    c.alias_classes_with_paths = alias_classes;
    return c;
  }

  static Contradiction
  makeDynamicTypeNeeded(const std::map<HeapPath, AbstractValue> &paths) {
    Contradiction c;
    c.kind = ContradictionKind::DynamicTypeNeeded;
    c.dynamic_type_paths = paths;
    return c;
  }

  static Contradiction
  makeFormalActualLength(unsigned formal_count, unsigned actual_count,
                         const std::vector<AbstractValue> &formals = {},
                         const std::vector<AbstractValue> &actuals = {}) {
    Contradiction c;
    c.kind = ContradictionKind::FormalActualLength;
    c.length_mismatch.formal_count = formal_count;
    c.length_mismatch.actual_count = actual_count;
    c.length_mismatch.formals = formals;
    c.length_mismatch.actuals = actuals;
    return c;
  }

  static Contradiction makeCapturedFormalActualLength(
      unsigned captured_formal_count, unsigned captured_actual_count,
      const std::vector<AbstractValue> &captured_formals = {},
      const std::vector<AbstractValue> &captured_actuals = {}) {
    Contradiction c;
    c.kind = ContradictionKind::CapturedFormalActualLength;
    c.captured_length_mismatch.captured_formal_count = captured_formal_count;
    c.captured_length_mismatch.captured_actual_count = captured_actual_count;
    c.captured_length_mismatch.captured_formals = captured_formals;
    c.captured_length_mismatch.captured_actuals = captured_actuals;
    return c;
  }
};

/**
 * Check for contradictions when applying a summary.
 * Returns None if no contradiction, Some(contradiction) if found.
 */
llvm::Optional<Contradiction> checkContradiction(
    const PulseFormula &caller_formula, const PulseFormula &callee_pre_formula,
    const std::map<AbstractValue, AbstractValue> &formal_to_actual_map,
    const std::map<AbstractValue, std::set<AbstractValue>>
        &actual_to_formals_map);

/**
 * Check for aliasing contradictions: distinct formals in pre that map to same
 * actual.
 */
llvm::Optional<Contradiction> checkAliasingContradiction(
    const std::map<AbstractValue, AbstractValue> &formal_to_actual_map,
    const PulseFormula &callee_pre_formula);

/**
 * Check if merged formulas are UNSAT (path condition contradiction).
 */
llvm::Optional<Contradiction>
checkPathConditionContradiction(const PulseFormula &caller_formula,
                                const PulseFormula &callee_pre_formula);

/**
 * Check for AliasingWithAllAliases contradiction
 * Collects all alias classes before raising contradiction
 */
llvm::Optional<Contradiction> checkAliasingWithAllAliases(
    const std::map<AbstractValue, AbstractValue> &formal_to_actual_map,
    const std::map<AbstractValue, std::set<AbstractValue>>
        &actual_to_formals_map,
    const PulseFormula &callee_pre_formula);

/**
 * Check for DynamicTypeNeeded contradiction
 * Returns map of heap paths to abstract values that need dynamic type
 * specialization
 */
llvm::Optional<Contradiction> checkDynamicTypeNeeded(
    const std::map<HeapPath, AbstractValue> &heap_paths_to_values);

/**
 * Check for CapturedFormalActualLength contradiction
 */
llvm::Optional<Contradiction> checkCapturedFormalActualLength(
    unsigned captured_formal_count, unsigned captured_actual_count,
    const std::vector<AbstractValue> &captured_formals = {},
    const std::vector<AbstractValue> &captured_actuals = {});

/**
 * Merge contradictions: when applying a summary with multiple disjuncts,
 * merge all possible contradictions into a single one.
 * Aligned with Infer's merge_contradictions.
 */
llvm::Optional<Contradiction>
mergeContradictions(const llvm::Optional<Contradiction> &c1,
                    const llvm::Optional<Contradiction> &c2);

/**
 * Check if contradiction is DynamicTypeNeeded and extract the map
 */
llvm::Optional<std::map<HeapPath, AbstractValue>>
isDynamicTypeNeededContradiction(const Contradiction &c);

/**
 * Enhanced contradiction checking with call_state support
 */
llvm::Optional<Contradiction> checkContradictionWithCallState(
    const PulseFormula &caller_formula, const PulseFormula &callee_pre_formula,
    const std::map<AbstractValue, AbstractValue> &formal_to_actual_map,
    const std::map<AbstractValue, std::set<AbstractValue>>
        &actual_to_formals_map,
    const CallState *call_state);

/**
 * Check for AliasingWithAllAliases with heap path support
 */
llvm::Optional<Contradiction> checkAliasingWithAllAliases(
    const std::map<AbstractValue, AbstractValue> &formal_to_actual_map,
    const std::map<AbstractValue, std::set<AbstractValue>>
        &actual_to_formals_map,
    const PulseFormula &callee_pre_formula, const CallState *call_state);

} // namespace pulse

#endif // CHECKER_PULSE_PULSECONTRADICTION_H
