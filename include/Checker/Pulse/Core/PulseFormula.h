#ifndef CHECKER_PULSE_PULSEFORMULA_H
#define CHECKER_PULSE_PULSEFORMULA_H

#include "Checker/Pulse/Core/PulseAbstractValue.h"

#include <map>
#include <set>
#include <vector>

#include <llvm/ADT/Optional.h>

namespace pulse {

class Substitution;

/**
 * Linear arithmetic term: represents a linear combination like 2*x + 3*y + 5
 * For now, simplified to track integer constraints
 */
struct LinearTerm {
  AbstractValue var;   // Variable (or constant if var is special)
  int64_t coefficient; // Coefficient (1 for variables, value for constants)

  LinearTerm(AbstractValue v, int64_t c) : var(v), coefficient(c) {}
  bool operator<(const LinearTerm &other) const {
    if (!(var == other.var))
      return var < other.var;
    return coefficient < other.coefficient;
  }
};

/**
 * Linear constraint: represents a linear inequality like 2*x + 3*y <= 5
 */
enum class ConstraintKind {
  LessEqual,    // <=
  Less,         // <
  GreaterEqual, // >=
  Greater,      // >
  Equal         // ==
};

struct LinearConstraint {
  std::vector<LinearTerm> terms; // Left-hand side terms
  int64_t constant;              // Right-hand side constant
  ConstraintKind kind;

  LinearConstraint() : constant(0), kind(ConstraintKind::Equal) {}
};

/**
 * PulseFormula: tracks path conditions including equalities, constraints, and
 * linear arithmetic. Enhanced with basic linear arithmetic reasoning following
 * Infer's design.
 */
class PulseFormula {
private:
  // Bottom flag: represents an unsatisfiable/contradictory path condition.
  // This is distinct from an empty formula (which represents "true").
  bool is_contradiction_{false};

  // Track equalities: v1 = v2
  // Use union-find structure: each value maps to its canonical representative
  std::map<AbstractValue, AbstractValue> equalities_;

  // Track disequalities: v1 != v2
  // Stored as canonical representative pairs, ordered (min, max)
  std::set<std::pair<AbstractValue, AbstractValue>> disequalities_;

  // Track non-null constraints: value is known to be non-null
  std::set<AbstractValue> non_null_values_;

  // Track null constraints: value is known to be null
  std::set<AbstractValue> null_values_;

  // Track integer constraints (linear arithmetic)
  std::vector<LinearConstraint> linear_constraints_;

  // Track integer bounds: lower_bound[v] <= v <= upper_bound[v]
  std::map<AbstractValue, int64_t> lower_bounds_;
  std::map<AbstractValue, int64_t> upper_bounds_;

  // Track if a value is known to be an integer
  std::set<AbstractValue> integer_values_;

  // Find canonical representative (with path compression). Mutates equalities_.
  AbstractValue findRep(AbstractValue v);

  // Follow equality chain without mutating. For use in const methods (e.g.
  // isConsistent).
  AbstractValue findRepReadOnly(AbstractValue v) const;

  static std::pair<AbstractValue, AbstractValue>
  normalizePair(AbstractValue v1, AbstractValue v2);

public:
  PulseFormula() = default;

  /**
   * Construct a contradiction (UNSAT) formula.
   */
  static PulseFormula contradiction() {
    PulseFormula f;
    f.is_contradiction_ = true;
    return f;
  }

  /**
   * Add equality constraint: v1 = v2
   * Returns true if consistent, false if contradicts existing constraints
   */
  bool addEquality(AbstractValue v1, AbstractValue v2);

  /**
   * Add disequality constraint: v1 != v2
   * Returns true if consistent, false if contradicts existing constraints
   */
  bool addDisequality(AbstractValue v1, AbstractValue v2);

  /**
   * Add non-null constraint: value is not null
   */
  void addNonNull(AbstractValue v);

  /**
   * Add null constraint: value is null
   * Returns true if consistent, false if contradicts non-null
   */
  bool addNull(AbstractValue v);

  /**
   * Get canonical representative of a value
   */
  AbstractValue getCanonical(AbstractValue v) const;

  /**
   * Check if value is known to be non-null
   */
  bool isNonNull(AbstractValue v) const;

  /**
   * Check if value is known to be null
   */
  bool isNull(AbstractValue v) const;

  /**
   * Check if two values are known to be equal
   */
  bool areEqual(AbstractValue v1, AbstractValue v2) const;

  /**
   * Check if two values are known to be different
   */
  bool areDisequal(AbstractValue v1, AbstractValue v2) const;

  /**
   * Check if formula is consistent (no contradictions)
   */
  bool isConsistent() const;

  /**
   * True if this is the contradiction element (UNSAT).
   */
  bool isContradiction() const { return is_contradiction_; }

  /**
   * True if any null constraint (ptr == null) is assumed on this path.
   * Used for isManifest: latent when we've assumed null.
   */
  bool hasNullConstraints() const;

  /**
   * True if path condition is empty or only non-null for allocated pointers.
   * Drives latent vs manifest reporting.
   */
  bool isEmptyOrTrivial() const;

  /**
   * Merge two formulas (for join operations)
   */
  static PulseFormula merge(const PulseFormula &f1, const PulseFormula &f2);

  /**
   * Join (disjunction) of two formulas: over-approximate f1 ∨ f2.
   * Keeps only facts that are stable across both branches (best-effort).
   *
   * This is the operation needed at CFG merge points in a Pulse-style engine:
   * joining control flow must not conjoin path conditions, otherwise feasible
   * bug witnesses are dropped (false negatives).
   */
  static PulseFormula join(const PulseFormula &f1, const PulseFormula &f2);

  /**
   * Clone formula
   */
  PulseFormula clone() const;

  /**
   * Apply a callee-to-caller substitution to all tracked values.
   */
  PulseFormula applySubstitution(const Substitution &substitution) const;

  /**
   * Compare two formulas after canonical normalization.
   * Used by loop handling to detect real convergence rather than matching only
   * a coarse shape.
   */
  bool equivalentTo(const PulseFormula &other) const;

  /**
   * Add integer constraint: mark value as integer type
   */
  void addIntegerConstraint(AbstractValue v);

  /**
   * Add linear constraint: e.g., 2*x + 3*y <= 5
   * Returns true if consistent, false if contradicts existing constraints
   */
  bool addLinearConstraint(const LinearConstraint &constraint);

  /**
   * Add bound constraint: lower <= v <= upper
   */
  bool addBounds(AbstractValue v, int64_t lower, int64_t upper);

  /**
   * Check if value is known to be an integer
   */
  bool isInteger(AbstractValue v) const;

  /**
   * Get lower bound for value (if known)
   */
  llvm::Optional<int64_t> getLowerBound(AbstractValue v) const;

  /**
   * Get upper bound for value (if known)
   */
  llvm::Optional<int64_t> getUpperBound(AbstractValue v) const;

  /**
   * Check if formula is UNSAT (unsatisfiable) using basic reasoning.
   * Full implementation would use SMT solver, but this provides basic checks.
   */
  bool isUnsat() const;

  /**
   * Simplify formula by propagating constraints
   */
  void simplify();

  /**
   * Add arithmetic operation result: result = op1 op op2
   * Supports: +, -, *, /, %, &, |, ^, <<, >>
   */
  bool addArithmeticOperation(AbstractValue result, AbstractValue op1,
                              AbstractValue op2, const std::string &op);

  /**
   * Add multiplication constraint: result = op1 * op2
   */
  bool addMultiply(AbstractValue result, AbstractValue op1, AbstractValue op2);

  /**
   * Add division constraint: result = op1 / op2
   */
  bool addDivide(AbstractValue result, AbstractValue op1, AbstractValue op2);

  /**
   * Add modulo constraint: result = op1 % op2
   */
  bool addModulo(AbstractValue result, AbstractValue op1, AbstractValue op2);

  /**
   * Add bitwise AND constraint: result = op1 & op2
   */
  bool addBitwiseAnd(AbstractValue result, AbstractValue op1,
                     AbstractValue op2);

  /**
   * Add bitwise OR constraint: result = op1 | op2
   */
  bool addBitwiseOr(AbstractValue result, AbstractValue op1, AbstractValue op2);

  /**
   * Add bitwise XOR constraint: result = op1 ^ op2
   */
  bool addBitwiseXor(AbstractValue result, AbstractValue op1,
                     AbstractValue op2);

  /**
   * Add left shift constraint: result = op1 << op2
   */
  bool addLeftShift(AbstractValue result, AbstractValue op1, AbstractValue op2);

  /**
   * Add right shift constraint: result = op1 >> op2
   */
  bool addRightShift(AbstractValue result, AbstractValue op1,
                     AbstractValue op2);

private:
  /**
   * Check if constraints are satisfiable using Z3
   */
  bool checkSatisfiability() const;

  /**
   * Propagate bounds through equalities
   */
  void propagateBounds();

  /**
   * Track arithmetic operations for Z3 encoding
   */
  struct ArithmeticOp {
    AbstractValue result;
    AbstractValue op1;
    AbstractValue op2;
    std::string op; // "+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>"
  };
  std::vector<ArithmeticOp> arithmetic_ops_;
};

} // namespace pulse

#endif // CHECKER_PULSE_PULSEFORMULA_H
