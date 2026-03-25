#include "Checker/Pulse/Core/PulseFormula.h"

#include "Checker/Pulse/Core/PulseSubstitution.h"
#include "Checker/Pulse/Report/PulseOptions.h"

#include <algorithm>
#include <limits>
#include <set>
#include <unordered_map>

#include <z3++.h>

namespace pulse {

namespace {
// Helper to convert AbstractValue to Z3 expression
z3::expr getZ3Var(z3::context &ctx, AbstractValue v,
                  std::unordered_map<AbstractValue, z3::expr> &var_map) {
  auto it = var_map.find(v);
  if (it != var_map.end()) {
    return it->second;
  }
  // Create integer variable for AbstractValue
  // We use the ID as part of the name to ensure uniqueness
  std::string name = "v_" + std::to_string(v.getId());
  z3::expr var = ctx.int_const(name.c_str());
  var_map.insert({v, var});
  return var;
}

static int64_t clampToI64(__int128 v) {
  if (v > static_cast<__int128>(std::numeric_limits<int64_t>::max())) {
    return std::numeric_limits<int64_t>::max();
  }
  if (v < static_cast<__int128>(std::numeric_limits<int64_t>::min())) {
    return std::numeric_limits<int64_t>::min();
  }
  return static_cast<int64_t>(v);
}

static int64_t satAddI64(int64_t a, int64_t b) {
  return clampToI64(static_cast<__int128>(a) + static_cast<__int128>(b));
}

static int64_t satMulI64(int64_t a, int64_t b) {
  return clampToI64(static_cast<__int128>(a) * static_cast<__int128>(b));
}
} // namespace

AbstractValue PulseFormula::findRep(AbstractValue v) {
  // Find root iteratively (avoid stack overflow on long chains)
  std::vector<AbstractValue> path;
  AbstractValue cur = v;
  while (true) {
    auto it = equalities_.find(cur);
    if (it == equalities_.end() || it->second == cur) {
      break;
    }
    path.push_back(cur);
    cur = it->second;
  }
  AbstractValue rep = cur;
  // Path compression: point all nodes on path to root
  for (AbstractValue u : path) {
    equalities_[u] = rep;
  }
  return rep;
}

AbstractValue PulseFormula::findRepReadOnly(AbstractValue v) const {
  AbstractValue cur = v;
  while (true) {
    auto it = equalities_.find(cur);
    if (it == equalities_.end() || it->second == cur) {
      return cur;
    }
    cur = it->second;
  }
}

std::pair<AbstractValue, AbstractValue>
PulseFormula::normalizePair(AbstractValue v1, AbstractValue v2) {
  if (v2 < v1) {
    std::swap(v1, v2);
  }
  return {v1, v2};
}

bool PulseFormula::addEquality(AbstractValue v1, AbstractValue v2) {
  if (is_contradiction_) {
    return false;
  }
  if (v1 == v2) {
    return true; // Trivial equality
  }

  AbstractValue rep1 = findRep(v1);
  AbstractValue rep2 = findRep(v2);

  if (rep1 == rep2) {
    return true; // Already equal
  }

  if (disequalities_.count(normalizePair(rep1, rep2)) > 0) {
    return false;
  }

  // Check for contradictions with null/non-null
  if (isNull(rep1) && isNonNull(rep2)) {
    return false; // Contradiction
  }
  if (isNonNull(rep1) && isNull(rep2)) {
    return false; // Contradiction
  }

  // Merge: make rep1 the canonical representative
  equalities_[rep2] = rep1;

  // Merge null/non-null sets
  if (isNull(rep2)) {
    null_values_.erase(rep2);
    null_values_.insert(rep1);
  }
  if (isNonNull(rep2)) {
    non_null_values_.erase(rep2);
    non_null_values_.insert(rep1);
  }

  if (!disequalities_.empty()) {
    std::set<std::pair<AbstractValue, AbstractValue>> updated;
    for (const auto &p : disequalities_) {
      AbstractValue a = p.first;
      AbstractValue b = p.second;
      if (a == rep2) {
        a = rep1;
      }
      if (b == rep2) {
        b = rep1;
      }
      AbstractValue ca = findRep(a);
      AbstractValue cb = findRep(b);
      if (ca == cb) {
        return false;
      }
      updated.insert(normalizePair(ca, cb));
    }
    disequalities_.swap(updated);
  }

  return true;
}

bool PulseFormula::addDisequality(AbstractValue v1, AbstractValue v2) {
  if (is_contradiction_) {
    return false;
  }
  AbstractValue rep1 = findRep(v1);
  AbstractValue rep2 = findRep(v2);
  if (rep1 == rep2) {
    return false;
  }
  disequalities_.insert(normalizePair(rep1, rep2));
  return true;
}

void PulseFormula::addNonNull(AbstractValue v) {
  if (is_contradiction_) {
    return;
  }
  AbstractValue rep = findRep(v);
  null_values_.erase(rep); // Remove if was null
  non_null_values_.insert(rep);
}

bool PulseFormula::addNull(AbstractValue v) {
  if (is_contradiction_) {
    return false;
  }
  AbstractValue rep = findRep(v);
  if (non_null_values_.count(rep) > 0) {
    return false; // Contradiction: already non-null
  }
  null_values_.insert(rep);
  return true;
}

AbstractValue PulseFormula::getCanonical(AbstractValue v) const {
  return const_cast<PulseFormula *>(this)->findRep(v);
}

bool PulseFormula::isNonNull(AbstractValue v) const {
  AbstractValue rep = const_cast<PulseFormula *>(this)->findRep(v);
  return non_null_values_.count(rep) > 0;
}

bool PulseFormula::isNull(AbstractValue v) const {
  AbstractValue rep = const_cast<PulseFormula *>(this)->findRep(v);
  return null_values_.count(rep) > 0;
}

bool PulseFormula::areEqual(AbstractValue v1, AbstractValue v2) const {
  if (v1 == v2) {
    return true;
  }
  AbstractValue rep1 = const_cast<PulseFormula *>(this)->findRep(v1);
  AbstractValue rep2 = const_cast<PulseFormula *>(this)->findRep(v2);
  return rep1 == rep2;
}

bool PulseFormula::areDisequal(AbstractValue v1, AbstractValue v2) const {
  AbstractValue rep1 = const_cast<PulseFormula *>(this)->findRep(v1);
  AbstractValue rep2 = const_cast<PulseFormula *>(this)->findRep(v2);
  if (rep1 == rep2) {
    return false;
  }
  return disequalities_.count(normalizePair(rep1, rep2)) > 0;
}

bool PulseFormula::isConsistent() const {
  if (is_contradiction_) {
    return false;
  }
  // Check for contradictions between null and non-null
  for (AbstractValue null_val : null_values_) {
    if (non_null_values_.count(null_val) > 0) {
      return false;
    }
  }
  for (const auto &p : disequalities_) {
    AbstractValue rep1 = findRepReadOnly(p.first);
    AbstractValue rep2 = findRepReadOnly(p.second);
    if (rep1 == rep2) {
      return false;
    }
  }
  return true;
}

bool PulseFormula::hasNullConstraints() const { return !null_values_.empty(); }

bool PulseFormula::isEmptyOrTrivial() const {
  if (!isConsistent())
    return false;
  // Manifest: no null constraints (we didn't assume ptr==null on this path)
  return !hasNullConstraints();
}

PulseFormula PulseFormula::merge(const PulseFormula &f1,
                                 const PulseFormula &f2) {
  if (f1.is_contradiction_) {
    return PulseFormula::contradiction();
  }
  if (f2.is_contradiction_) {
    return PulseFormula::contradiction();
  }
  PulseFormula result = f1.clone();

  // Add equalities from f2
  for (const auto &eq : f2.equalities_) {
    if (!result.addEquality(eq.first, eq.second)) {
      return PulseFormula::contradiction();
    }
  }

  for (const auto &neq : f2.disequalities_) {
    if (!result.addDisequality(neq.first, neq.second)) {
      return PulseFormula::contradiction();
    }
  }

  // Add non-null constraints from f2
  for (AbstractValue v : f2.non_null_values_) {
    result.addNonNull(v);
  }

  // Add null constraints from f2 (checking for contradictions)
  for (AbstractValue v : f2.null_values_) {
    if (!result.addNull(v)) {
      // Contradiction detected
      return PulseFormula::contradiction();
    }
  }

  // Merge linear constraints
  for (const auto &constraint : f2.linear_constraints_) {
    if (!result.addLinearConstraint(constraint)) {
      return PulseFormula::contradiction();
    }
  }

  // Merge bounds (take intersection)
  for (const auto &kv : f2.lower_bounds_) {
    auto it = result.lower_bounds_.find(kv.first);
    if (it != result.lower_bounds_.end()) {
      result.lower_bounds_[kv.first] = std::max(it->second, kv.second);
    } else {
      result.lower_bounds_[kv.first] = kv.second;
    }
  }

  for (const auto &kv : f2.upper_bounds_) {
    auto it = result.upper_bounds_.find(kv.first);
    if (it != result.upper_bounds_.end()) {
      result.upper_bounds_[kv.first] = std::min(it->second, kv.second);
    } else {
      result.upper_bounds_[kv.first] = kv.second;
    }
  }

  // Check bounds consistency after merge
  for (const auto &kv : result.lower_bounds_) {
    auto upper_it = result.upper_bounds_.find(kv.first);
    if (upper_it != result.upper_bounds_.end()) {
      if (kv.second > upper_it->second) {
        return PulseFormula::contradiction();
      }
    }
  }

  // Merge integer markers
  for (AbstractValue v : f2.integer_values_) {
    result.addIntegerConstraint(v);
  }

  return result;
}

PulseFormula PulseFormula::join(const PulseFormula &f1,
                                const PulseFormula &f2) {
  // f1 ∨ f2: if one side is UNSAT, return the other side.
  if (f1.is_contradiction_) {
    return f2.clone();
  }
  if (f2.is_contradiction_) {
    return f1.clone();
  }

  PulseFormula out;

  // Keep only constraints that are stable across both sides.
  // This is a best-effort disjunction approximation: it intentionally
  // forgets most relational information rather than (incorrectly) conjoining.

  // Null/non-null: intersection.
  for (AbstractValue v : f1.null_values_) {
    if (f2.null_values_.count(v) > 0) {
      (void)out.addNull(v);
    }
  }
  for (AbstractValue v : f1.non_null_values_) {
    if (f2.non_null_values_.count(v) > 0) {
      out.addNonNull(v);
    }
  }

  // Disequalities: intersection (already normalized pairs).
  for (const auto &p : f1.disequalities_) {
    if (f2.disequalities_.count(p) > 0) {
      (void)out.addDisequality(p.first, p.second);
    }
  }

  // Bounds: if both sides have a bound for the same canonical key, keep the
  // weakest one implied by the disjunction.
  for (const auto &kv : f1.lower_bounds_) {
    auto it2 = f2.lower_bounds_.find(kv.first);
    if (it2 != f2.lower_bounds_.end()) {
      out.lower_bounds_[kv.first] = std::min(kv.second, it2->second);
    }
  }
  for (const auto &kv : f1.upper_bounds_) {
    auto it2 = f2.upper_bounds_.find(kv.first);
    if (it2 != f2.upper_bounds_.end()) {
      out.upper_bounds_[kv.first] = std::max(kv.second, it2->second);
    }
  }
  for (const auto &kv : out.lower_bounds_) {
    auto it_u = out.upper_bounds_.find(kv.first);
    if (it_u != out.upper_bounds_.end() && kv.second > it_u->second) {
      // Shouldn't happen from min/max above, but be defensive.
      return PulseFormula::contradiction();
    }
  }

  // Integer markers: intersection.
  for (AbstractValue v : f1.integer_values_) {
    if (f2.integer_values_.count(v) > 0) {
      out.addIntegerConstraint(v);
    }
  }

  // Linear constraints: keep syntactic intersection (normalized).
  auto normalize_constraint =
      [](const PulseFormula &f, const LinearConstraint &c) -> LinearConstraint {
    LinearConstraint out_c;
    out_c.constant = c.constant;
    out_c.kind = c.kind;
    out_c.terms = c.terms;
    for (auto &t : out_c.terms) {
      t.var = f.findRepReadOnly(t.var);
    }
    std::sort(out_c.terms.begin(), out_c.terms.end());
    return out_c;
  };

  std::multiset<std::string> sigs1;
  auto signature = [](const LinearConstraint &c) -> std::string {
    std::string s;
    s.reserve(64);
    s += std::to_string(static_cast<int>(c.kind));
    s += ":";
    s += std::to_string(c.constant);
    for (const auto &t : c.terms) {
      s += "|";
      s += std::to_string(t.var.getId());
      s += ",";
      s += std::to_string(t.coefficient);
    }
    return s;
  };

  for (const auto &c : f1.linear_constraints_) {
    LinearConstraint nc = normalize_constraint(f1, c);
    sigs1.insert(signature(nc));
  }
  for (const auto &c : f2.linear_constraints_) {
    LinearConstraint nc = normalize_constraint(f2, c);
    std::string sig = signature(nc);
    auto it = sigs1.find(sig);
    if (it != sigs1.end()) {
      sigs1.erase(it);
      // Add the normalized constraint to out.
      // This should not introduce contradictions because it holds on both
      // sides, but still run through addLinearConstraint for internal
      // bookkeeping.
      if (!out.addLinearConstraint(nc)) {
        return PulseFormula::contradiction();
      }
    }
  }

  // Equalities and arithmetic ops are intentionally forgotten for now: the
  // disjunction of equalities requires disjunctive formulas, and keeping them
  // unsafely would reintroduce the original join bug.

  return out;
}

PulseFormula PulseFormula::clone() const {
  PulseFormula f;
  f.is_contradiction_ = is_contradiction_;
  f.equalities_ = equalities_;
  f.disequalities_ = disequalities_;
  f.non_null_values_ = non_null_values_;
  f.null_values_ = null_values_;
  f.linear_constraints_ = linear_constraints_;
  f.lower_bounds_ = lower_bounds_;
  f.upper_bounds_ = upper_bounds_;
  f.integer_values_ = integer_values_;
  f.arithmetic_ops_ = arithmetic_ops_;
  return f;
}

PulseFormula
PulseFormula::applySubstitution(const Substitution &substitution) const {
  PulseFormula out;
  if (is_contradiction_) {
    return PulseFormula::contradiction();
  }
  for (const auto &eq : equalities_) {
    AbstractValue lhs = substitution.substituteOrIdentity(eq.first);
    AbstractValue rhs = substitution.substituteOrIdentity(eq.second);
    if (!out.addEquality(lhs, rhs)) {
      return PulseFormula::contradiction();
    }
  }
  for (const auto &neq : disequalities_) {
    AbstractValue lhs = substitution.substituteOrIdentity(neq.first);
    AbstractValue rhs = substitution.substituteOrIdentity(neq.second);
    if (!out.addDisequality(lhs, rhs)) {
      return PulseFormula::contradiction();
    }
  }
  for (AbstractValue v : non_null_values_) {
    out.addNonNull(substitution.substituteOrIdentity(v));
  }
  for (AbstractValue v : null_values_) {
    if (!out.addNull(substitution.substituteOrIdentity(v))) {
      return PulseFormula::contradiction();
    }
  }

  // Apply substitution to linear constraints
  for (const auto &constraint : linear_constraints_) {
    LinearConstraint new_constraint;
    new_constraint.constant = constraint.constant;
    new_constraint.kind = constraint.kind;
    for (const auto &term : constraint.terms) {
      AbstractValue new_var = substitution.substituteOrIdentity(term.var);
      new_constraint.terms.emplace_back(new_var, term.coefficient);
    }
    if (!out.addLinearConstraint(new_constraint)) {
      return PulseFormula::contradiction();
    }
  }

  // Apply substitution to bounds
  for (const auto &kv : lower_bounds_) {
    AbstractValue new_var = substitution.substituteOrIdentity(kv.first);
    out.lower_bounds_[new_var] = kv.second;
  }
  for (const auto &kv : upper_bounds_) {
    AbstractValue new_var = substitution.substituteOrIdentity(kv.first);
    out.upper_bounds_[new_var] = kv.second;
  }

  // Apply substitution to integer markers
  for (AbstractValue v : integer_values_) {
    out.addIntegerConstraint(substitution.substituteOrIdentity(v));
  }

  return out;
}

bool PulseFormula::equivalentTo(const PulseFormula &other) const {
  auto normalize_value_set = [](const PulseFormula &formula,
                                const std::set<AbstractValue> &values) {
    std::set<AbstractValue> normalized;
    for (AbstractValue v : values) {
      normalized.insert(formula.findRepReadOnly(v));
    }
    return normalized;
  };

  auto normalize_pairs = [](const PulseFormula &formula,
                            const std::set<std::pair<AbstractValue,
                                                     AbstractValue>> &pairs) {
    std::set<std::pair<AbstractValue, AbstractValue>> normalized;
    for (const auto &p : pairs) {
      normalized.insert(normalizePair(formula.findRepReadOnly(p.first),
                                      formula.findRepReadOnly(p.second)));
    }
    return normalized;
  };

  auto normalize_bounds =
      [](const PulseFormula &formula,
         const std::map<AbstractValue, int64_t> &bounds) {
        std::map<AbstractValue, int64_t> normalized;
        for (const auto &kv : bounds) {
          normalized[formula.findRepReadOnly(kv.first)] = kv.second;
        }
        return normalized;
      };

  auto normalize_linear_constraints = [](const PulseFormula &formula) {
    std::multiset<std::string> normalized;
    for (const auto &constraint : formula.linear_constraints_) {
      std::vector<LinearTerm> terms = constraint.terms;
      for (auto &term : terms) {
        term.var = formula.findRepReadOnly(term.var);
      }
      std::sort(terms.begin(), terms.end());

      std::string sig;
      sig.reserve(64);
      sig += std::to_string(static_cast<int>(constraint.kind));
      sig += ":";
      sig += std::to_string(constraint.constant);
      for (const auto &term : terms) {
        sig += "|";
        sig += std::to_string(term.var.getId());
        sig += ",";
        sig += std::to_string(term.coefficient);
      }
      normalized.insert(std::move(sig));
    }
    return normalized;
  };

  auto normalize_arithmetic_ops = [](const PulseFormula &formula) {
    std::multiset<std::string> normalized;
    for (const auto &op : formula.arithmetic_ops_) {
      std::string sig;
      sig.reserve(48);
      sig += std::to_string(formula.findRepReadOnly(op.result).getId());
      sig += ":";
      sig += std::to_string(formula.findRepReadOnly(op.op1).getId());
      sig += ":";
      sig += std::to_string(formula.findRepReadOnly(op.op2).getId());
      sig += ":";
      sig += op.op;
      normalized.insert(std::move(sig));
    }
    return normalized;
  };

  auto normalize_equalities = [](const PulseFormula &formula) {
    std::set<std::pair<AbstractValue, AbstractValue>> normalized;
    std::set<AbstractValue> values;
    for (const auto &eq : formula.equalities_) {
      values.insert(eq.first);
      values.insert(eq.second);
    }
    for (AbstractValue value : values) {
      AbstractValue rep = formula.findRepReadOnly(value);
      if (!(value == rep)) {
        normalized.insert(normalizePair(value, rep));
      }
    }
    return normalized;
  };

  return is_contradiction_ == other.is_contradiction_ &&
         normalize_equalities(*this) == normalize_equalities(other) &&
         normalize_pairs(*this, disequalities_) ==
             normalize_pairs(other, other.disequalities_) &&
         normalize_value_set(*this, null_values_) ==
             normalize_value_set(other, other.null_values_) &&
         normalize_value_set(*this, non_null_values_) ==
             normalize_value_set(other, other.non_null_values_) &&
         normalize_bounds(*this, lower_bounds_) ==
             normalize_bounds(other, other.lower_bounds_) &&
         normalize_bounds(*this, upper_bounds_) ==
             normalize_bounds(other, other.upper_bounds_) &&
         normalize_value_set(*this, integer_values_) ==
             normalize_value_set(other, other.integer_values_) &&
         normalize_linear_constraints(*this) ==
             normalize_linear_constraints(other) &&
         normalize_arithmetic_ops(*this) == normalize_arithmetic_ops(other);
}

void PulseFormula::addIntegerConstraint(AbstractValue v) {
  if (is_contradiction_) {
    return;
  }
  AbstractValue rep = findRep(v);
  integer_values_.insert(rep);
}

bool PulseFormula::addLinearConstraint(const LinearConstraint &constraint) {
  if (is_contradiction_) {
    return false;
  }
  // Simplified implementation: basic consistency checking
  // Full implementation would use simplex algorithm or SMT solver

  // For now, check if constraint contradicts existing bounds
  int64_t min_value = 0, max_value = 0;
  bool has_bounds = false;

  for (const auto &term : constraint.terms) {
    AbstractValue rep = findRep(term.var);
    auto lower_it = lower_bounds_.find(rep);
    auto upper_it = upper_bounds_.find(rep);

    int64_t term_min = (lower_it != lower_bounds_.end())
                           ? lower_it->second
                           : std::numeric_limits<int64_t>::min();
    int64_t term_max = (upper_it != upper_bounds_.end())
                           ? upper_it->second
                           : std::numeric_limits<int64_t>::max();

    // Interval arithmetic with saturation to avoid UB/overflow.
    if (term.coefficient > 0) {
      min_value = satAddI64(min_value, satMulI64(term.coefficient, term_min));
      max_value = satAddI64(max_value, satMulI64(term.coefficient, term_max));
    } else {
      min_value = satAddI64(min_value, satMulI64(term.coefficient, term_max));
      max_value = satAddI64(max_value, satMulI64(term.coefficient, term_min));
    }
    has_bounds = true;
  }

  if (has_bounds) {
    int64_t rhs = constraint.constant;
    switch (constraint.kind) {
    case ConstraintKind::LessEqual:
      if (min_value > rhs)
        return false; // Contradiction
      break;
    case ConstraintKind::Less:
      if (min_value >= rhs)
        return false;
      break;
    case ConstraintKind::GreaterEqual:
      if (max_value < rhs)
        return false;
      break;
    case ConstraintKind::Greater:
      if (max_value <= rhs)
        return false;
      break;
    case ConstraintKind::Equal:
      if (min_value > rhs || max_value < rhs)
        return false;
      break;
    }
  }

  linear_constraints_.push_back(constraint);

  // Use Z3 for full consistency check
  return checkSatisfiability();
}

bool PulseFormula::addBounds(AbstractValue v, int64_t lower, int64_t upper) {
  if (is_contradiction_) {
    return false;
  }
  if (lower > upper) {
    return false; // Invalid bounds
  }

  AbstractValue rep = findRep(v);

  // Check against existing bounds
  auto lower_it = lower_bounds_.find(rep);
  auto upper_it = upper_bounds_.find(rep);

  if (lower_it != lower_bounds_.end()) {
    // Tighten lower bound (take max).
    lower_bounds_[rep] = std::max(lower_it->second, lower);
  } else {
    lower_bounds_[rep] = lower;
  }

  if (upper_it != upper_bounds_.end()) {
    // Tighten upper bound (take min).
    upper_bounds_[rep] = std::min(upper_it->second, upper);
  } else {
    upper_bounds_[rep] = upper;
  }

  // Check consistency
  if (lower_bounds_[rep] > upper_bounds_[rep]) {
    return false; // Contradiction
  }

  return true;
}

bool PulseFormula::isInteger(AbstractValue v) const {
  AbstractValue rep = const_cast<PulseFormula *>(this)->findRep(v);
  return integer_values_.count(rep) > 0;
}

llvm::Optional<int64_t> PulseFormula::getLowerBound(AbstractValue v) const {
  AbstractValue rep = const_cast<PulseFormula *>(this)->findRep(v);
  auto it = lower_bounds_.find(rep);
  if (it != lower_bounds_.end()) {
    return it->second;
  }
  return llvm::None;
}

llvm::Optional<int64_t> PulseFormula::getUpperBound(AbstractValue v) const {
  AbstractValue rep = const_cast<PulseFormula *>(this)->findRep(v);
  auto it = upper_bounds_.find(rep);
  if (it != upper_bounds_.end()) {
    return it->second;
  }
  return llvm::None;
}

bool PulseFormula::isUnsat() const {
  if (is_contradiction_) {
    return true;
  }
  // Check satisfiability using Z3
  return !checkSatisfiability();
}

bool PulseFormula::checkSatisfiability() const {
  if (is_contradiction_) {
    return false;
  }
  // Early exit if formula is empty
  if (linear_constraints_.empty() && lower_bounds_.empty() &&
      upper_bounds_.empty() && equalities_.empty() && disequalities_.empty() &&
      null_values_.empty() && non_null_values_.empty() &&
      arithmetic_ops_.empty()) {
    return true;
  }

  // Quick consistency check: if we have both null and non-null for same value,
  // it's UNSAT
  for (AbstractValue null_val : null_values_) {
    AbstractValue rep = const_cast<PulseFormula *>(this)->findRep(null_val);
    if (non_null_values_.count(rep) > 0) {
      return false; // Contradiction: null and non-null
    }
  }

  // Fast mode: skip Z3 entirely; assume satisfiable so paths are not pruned by
  // SMT
  if (pulse::options::disableSMT()) {
    return true;
  }

  try {
    z3::context ctx;
    z3::solver solver(ctx);
    std::unordered_map<AbstractValue, z3::expr> var_map;

    // Add equality constraints first (build equivalence classes)
    // Use a union-find approach: track canonical representatives
    std::map<AbstractValue, AbstractValue> canon_map;
    for (const auto &eq : equalities_) {
      AbstractValue rep1 = const_cast<PulseFormula *>(this)->findRep(eq.first);
      AbstractValue rep2 = const_cast<PulseFormula *>(this)->findRep(eq.second);
      if (!(rep1 == rep2)) {
        // Use the smaller value as canonical representative
        AbstractValue canon = (rep1 < rep2) ? rep1 : rep2;
        AbstractValue other = (rep1 < rep2) ? rep2 : rep1;
        canon_map[other] = canon;

        // Add equality constraint to Z3
        z3::expr v1 = getZ3Var(ctx, canon, var_map);
        z3::expr v2 = getZ3Var(ctx, other, var_map);
        solver.add(v1 == v2);
      }
    }

    // Helper to get canonical representative
    auto getCanon = [&](AbstractValue v) -> AbstractValue {
      AbstractValue rep = const_cast<PulseFormula *>(this)->findRep(v);
      auto it = canon_map.find(rep);
      return (it != canon_map.end()) ? it->second : rep;
    };

    // Add linear constraints (using canonical representatives)
    for (const auto &constraint : linear_constraints_) {
      z3::expr lhs = ctx.int_val(0);
      for (const auto &term : constraint.terms) {
        AbstractValue canon = getCanon(term.var);
        z3::expr var = getZ3Var(ctx, canon, var_map);
        lhs = lhs + (ctx.int_val(term.coefficient) * var);
      }

      z3::expr rhs = ctx.int_val(constraint.constant);

      switch (constraint.kind) {
      case ConstraintKind::LessEqual:
        solver.add(lhs <= rhs);
        break;
      case ConstraintKind::Less:
        solver.add(lhs < rhs);
        break;
      case ConstraintKind::GreaterEqual:
        solver.add(lhs >= rhs);
        break;
      case ConstraintKind::Greater:
        solver.add(lhs > rhs);
        break;
      case ConstraintKind::Equal:
        solver.add(lhs == rhs);
        break;
      }
    }

    // Add bounds constraints (using canonical representatives)
    for (const auto &kv : lower_bounds_) {
      AbstractValue canon = getCanon(kv.first);
      z3::expr var = getZ3Var(ctx, canon, var_map);
      solver.add(var >= ctx.int_val(kv.second));
    }

    for (const auto &kv : upper_bounds_) {
      AbstractValue canon = getCanon(kv.first);
      z3::expr var = getZ3Var(ctx, canon, var_map);
      solver.add(var <= ctx.int_val(kv.second));
    }

    // Add disequality constraints (using canonical representatives)
    for (const auto &neq : disequalities_) {
      AbstractValue rep1 = const_cast<PulseFormula *>(this)->findRep(neq.first);
      AbstractValue rep2 =
          const_cast<PulseFormula *>(this)->findRep(neq.second);
      AbstractValue canon1 = getCanon(rep1);
      AbstractValue canon2 = getCanon(rep2);

      // If they're equal after canonicalization, contradiction
      if (canon1 == canon2) {
        return false;
      }

      z3::expr v1 = getZ3Var(ctx, canon1, var_map);
      z3::expr v2 = getZ3Var(ctx, canon2, var_map);
      solver.add(v1 != v2);
    }

    // Add null/non-null constraints (treating null as 0)
    for (AbstractValue v : null_values_) {
      AbstractValue canon = getCanon(v);
      z3::expr var = getZ3Var(ctx, canon, var_map);
      solver.add(var == ctx.int_val(0));
    }

    for (AbstractValue v : non_null_values_) {
      AbstractValue canon = getCanon(v);
      z3::expr var = getZ3Var(ctx, canon, var_map);
      solver.add(var != ctx.int_val(0));
    }

    // Add arithmetic operations (using canonical representatives)
    for (const auto &arith_op : arithmetic_ops_) {
      AbstractValue canon_result = getCanon(arith_op.result);
      AbstractValue canon_op1 = getCanon(arith_op.op1);
      AbstractValue canon_op2 = getCanon(arith_op.op2);

      z3::expr result_var = getZ3Var(ctx, canon_result, var_map);
      z3::expr op1_var = getZ3Var(ctx, canon_op1, var_map);
      z3::expr op2_var = getZ3Var(ctx, canon_op2, var_map);

      if (arith_op.op == "+") {
        solver.add(result_var == op1_var + op2_var);
      } else if (arith_op.op == "-") {
        solver.add(result_var == op1_var - op2_var);
      } else if (arith_op.op == "*") {
        solver.add(result_var == op1_var * op2_var);
      } else if (arith_op.op == "/") {
        // Division: result = op1 / op2, with op2 != 0
        solver.add(op2_var != 0);
        solver.add(result_var == op1_var / op2_var);
      } else if (arith_op.op == "%") {
        // Modulo: result = op1 % op2, with op2 != 0
        solver.add(op2_var != 0);
        solver.add(result_var == z3::mod(op1_var, op2_var));
      } else if (arith_op.op == "&") {
        // Bitwise operators are not defined on Z3 Int sort. We currently
        // model variables as Ints, so skip these constraints rather than
        // crashing and falling back to a much weaker satisfiability check.
        continue;
      } else if (arith_op.op == "|") {
        continue;
      } else if (arith_op.op == "^") {
        continue;
      } else if (arith_op.op == "<<") {
        continue;
      } else if (arith_op.op == ">>") {
        continue;
      }
    }

    // Check satisfiability with timeout (10 seconds)
    z3::params p(ctx);
    p.set(":timeout", 10000u);
    solver.set(p);

    z3::check_result result = solver.check();
    return result == z3::sat;

  } catch (const z3::exception &e) {
    // Fallback: if solver fails, check basic consistency
    // This is conservative - we assume satisfiable if solver fails
    return isConsistent();
  }
}

void PulseFormula::simplify() { propagateBounds(); }

void PulseFormula::propagateBounds() {
  // Propagate bounds through equalities
  // If v1 = v2, then bounds(v1) should equal bounds(v2)
  for (const auto &eq : equalities_) {
    AbstractValue rep1 = findRep(eq.first);
    AbstractValue rep2 = findRep(eq.second);

    if (rep1 == rep2)
      continue;

    // Merge bounds
    auto lower1 = lower_bounds_.find(rep1);
    auto upper1 = upper_bounds_.find(rep1);
    auto lower2 = lower_bounds_.find(rep2);
    auto upper2 = upper_bounds_.find(rep2);

    if (lower1 != lower_bounds_.end() || lower2 != lower_bounds_.end()) {
      int64_t new_lower = std::numeric_limits<int64_t>::min();
      if (lower1 != lower_bounds_.end())
        new_lower = std::max(new_lower, lower1->second);
      if (lower2 != lower_bounds_.end())
        new_lower = std::max(new_lower, lower2->second);
      if (new_lower != std::numeric_limits<int64_t>::min()) {
        lower_bounds_[rep1] = new_lower;
        lower_bounds_[rep2] = new_lower;
      }
    }

    if (upper1 != upper_bounds_.end() || upper2 != upper_bounds_.end()) {
      int64_t new_upper = std::numeric_limits<int64_t>::max();
      if (upper1 != upper_bounds_.end())
        new_upper = std::min(new_upper, upper1->second);
      if (upper2 != upper_bounds_.end())
        new_upper = std::min(new_upper, upper2->second);
      if (new_upper != std::numeric_limits<int64_t>::max()) {
        upper_bounds_[rep1] = new_upper;
        upper_bounds_[rep2] = new_upper;
      }
    }
  }
}

bool PulseFormula::addArithmeticOperation(AbstractValue result,
                                          AbstractValue op1, AbstractValue op2,
                                          const std::string &op) {
  if (op == "*")
    return addMultiply(result, op1, op2);
  if (op == "/")
    return addDivide(result, op1, op2);
  if (op == "%")
    return addModulo(result, op1, op2);
  if (op == "&")
    return addBitwiseAnd(result, op1, op2);
  if (op == "|")
    return addBitwiseOr(result, op1, op2);
  if (op == "^")
    return addBitwiseXor(result, op1, op2);
  if (op == "<<")
    return addLeftShift(result, op1, op2);
  if (op == ">>")
    return addRightShift(result, op1, op2);

  // For + and -, use linear constraints
  if (op == "+" || op == "-") {
    LinearConstraint constraint;
    // result = op1 (+|-) op2  <=>  result - op1 -/+ op2 = 0
    constraint.terms.emplace_back(result, 1);
    constraint.terms.emplace_back(op1, -1);
    constraint.terms.emplace_back(op2, op == "+" ? -1 : 1);
    constraint.constant = 0;
    constraint.kind = ConstraintKind::Equal;
    return addLinearConstraint(constraint);
  }

  return false;
}

bool PulseFormula::addMultiply(AbstractValue result, AbstractValue op1,
                               AbstractValue op2) {
  if (is_contradiction_) {
    return false;
  }
  ArithmeticOp op;
  op.result = result;
  op.op1 = op1;
  op.op2 = op2;
  op.op = "*";
  arithmetic_ops_.push_back(op);
  return checkSatisfiability();
}

bool PulseFormula::addDivide(AbstractValue result, AbstractValue op1,
                             AbstractValue op2) {
  if (is_contradiction_) {
    return false;
  }
  // Check division by zero
  auto lower_opt = getLowerBound(op2);
  auto upper_opt = getUpperBound(op2);
  bool is_zero = (lower_opt && *lower_opt == 0 && upper_opt && *upper_opt == 0);
  if (isNull(op2) || (isInteger(op2) && is_zero)) {
    return false; // Division by zero
  }
  ArithmeticOp op;
  op.result = result;
  op.op1 = op1;
  op.op2 = op2;
  op.op = "/";
  arithmetic_ops_.push_back(op);
  return checkSatisfiability();
}

bool PulseFormula::addModulo(AbstractValue result, AbstractValue op1,
                             AbstractValue op2) {
  if (is_contradiction_) {
    return false;
  }
  // Check modulo by zero
  auto lower_opt = getLowerBound(op2);
  auto upper_opt = getUpperBound(op2);
  bool is_zero = (lower_opt && *lower_opt == 0 && upper_opt && *upper_opt == 0);
  if (isNull(op2) || (isInteger(op2) && is_zero)) {
    return false; // Modulo by zero
  }
  ArithmeticOp op;
  op.result = result;
  op.op1 = op1;
  op.op2 = op2;
  op.op = "%";
  arithmetic_ops_.push_back(op);
  return checkSatisfiability();
}

bool PulseFormula::addBitwiseAnd(AbstractValue result, AbstractValue op1,
                                 AbstractValue op2) {
  if (is_contradiction_) {
    return false;
  }
  // Bitwise operators are currently not modeled in the satisfiability check
  // (we use Z3 Int sort). Keep analysis stable by recording only that the
  // values are integers and not introducing a potentially explosive op log.
  addIntegerConstraint(result);
  addIntegerConstraint(op1);
  addIntegerConstraint(op2);
  return true;
}

bool PulseFormula::addBitwiseOr(AbstractValue result, AbstractValue op1,
                                AbstractValue op2) {
  if (is_contradiction_) {
    return false;
  }
  addIntegerConstraint(result);
  addIntegerConstraint(op1);
  addIntegerConstraint(op2);
  return true;
}

bool PulseFormula::addBitwiseXor(AbstractValue result, AbstractValue op1,
                                 AbstractValue op2) {
  if (is_contradiction_) {
    return false;
  }
  addIntegerConstraint(result);
  addIntegerConstraint(op1);
  addIntegerConstraint(op2);
  return true;
}

bool PulseFormula::addLeftShift(AbstractValue result, AbstractValue op1,
                                AbstractValue op2) {
  if (is_contradiction_) {
    return false;
  }
  addIntegerConstraint(result);
  addIntegerConstraint(op1);
  addIntegerConstraint(op2);
  return true;
}

bool PulseFormula::addRightShift(AbstractValue result, AbstractValue op1,
                                 AbstractValue op2) {
  if (is_contradiction_) {
    return false;
  }
  addIntegerConstraint(result);
  addIntegerConstraint(op1);
  addIntegerConstraint(op2);
  return true;
}

} // namespace pulse
