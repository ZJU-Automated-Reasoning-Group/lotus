#ifndef CHECKER_PULSE_PULSEJOIN_H
#define CHECKER_PULSE_PULSEJOIN_H

#include "Checker/Pulse/Core/PulsePathContext.h"
#include "Checker/Pulse/Domain/PulseDomain.h"
#include "Checker/Pulse/Domain/PulseOperations.h"

#include <map>
#include <set>
#include <tuple>

#include <llvm/ADT/Optional.h>

namespace pulse {

/**
 * PulseJoin: Production-ready join operation matching Infer's design.
 *
 * Joins two abstract states, handling:
 * - Value substitution and canonicalization
 * - Heap graph merging
 * - Attribute joining (one-sided vs two-sided)
 * - Path context merging
 * - Formula joining
 */
class PulseJoin {
public:
  /**
   * Join two abductive domains with their path contexts.
   * Returns joined domain and path context, or None if join fails.
   */
  static llvm::Optional<std::pair<AbductiveDomain, PathContext>>
  join(const AbductiveDomain &lhs, const PathContext &path_lhs,
       const AbductiveDomain &rhs, const PathContext &path_rhs);

  /**
   * Join two abductive domains (without path context).
   * Returns joined domain or None if join fails.
   */
  static llvm::Optional<AbductiveDomain>
  joinAbductive(const AbductiveDomain &lhs, const AbductiveDomain &rhs);

  /**
   * Join two summaries.
   */
  static AbductiveDomain joinSummaries(const AbductiveDomain &lhs,
                                       const AbductiveDomain &rhs);

private:
  // Join state: tracks substitutions during join
  struct JoinState {
    // Maps (lhs_value, rhs_value) -> joined_value
    std::map<
        std::pair<llvm::Optional<AbstractValue>, llvm::Optional<AbstractValue>>,
        AbstractValue>
        subst;

    // Reverse map: joined_value -> (lhs_value, rhs_value)
    std::map<AbstractValue, std::pair<llvm::Optional<AbstractValue>,
                                      llvm::Optional<AbstractValue>>>
        rev_subst;

    // Visited pairs to avoid cycles
    std::set<
        std::pair<llvm::Optional<AbstractValue>, llvm::Optional<AbstractValue>>>
        visited;

    AbstractValueFactory *factory;

    JoinState(AbstractValueFactory *f) : factory(f) {}
  };

  /**
   * Join two values with their histories.
   * Returns (join_state, (joined_value, joined_history))
   */
  static std::pair<JoinState &, std::pair<AbstractValue, ValueHistory>>
  joinValuesHists(JoinState &state, const AbstractValue &lhs_val,
                  const ValueHistory &lhs_hist, const AbstractValue &rhs_val,
                  const ValueHistory &rhs_hist);

  /**
   * Join optional values (handles one-sided cases).
   */
  static std::pair<JoinState &, std::pair<AbstractValue, ValueHistory>>
  joinValuesHistsOpts(
      JoinState &state, const AbductiveDomain &lhs_astate,
      llvm::Optional<std::pair<AbstractValue, ValueHistory>> lhs_opt,
      const AbductiveDomain &rhs_astate,
      llvm::Optional<std::pair<AbstractValue, ValueHistory>> rhs_opt);

  /**
   * Join heaps recursively.
   */
  static std::pair<JoinState &, Heap>
  joinHeaps(JoinState &state, Heap &heap_join,
            const AbductiveDomain &lhs_astate,
            llvm::Optional<std::pair<AbstractValue, ValueHistory>> lhs_opt,
            const AbductiveDomain &rhs_astate,
            llvm::Optional<std::pair<AbstractValue, ValueHistory>> rhs_opt);

  /**
   * Join stacks.
   */
  static std::pair<JoinState &, std::pair<Stack, Heap>>
  joinStacks(JoinState &state, const AbductiveDomain &lhs_astate,
             const AbductiveDomain &rhs_astate);

  /**
   * Join attributes: handles one-sided vs two-sided cases.
   */
  static AttributeSet
  joinAttributes(JoinState &state, const AbductiveDomain &lhs_astate,
                 const AbductiveDomain &rhs_astate, bool use_pre_attrs,
                 AbstractValue joined_addr,
                 llvm::Optional<AbstractValue> lhs_addr_opt,
                 llvm::Optional<AbstractValue> rhs_addr_opt);

  /**
   * Join one-sided attribute (only in one branch).
   */
  static llvm::Optional<Attribute> joinOneSidedAttribute(Attribute attr);

  /**
   * Join two-sided attribute (in both branches).
   */
  static llvm::Optional<Attribute>
  joinTwoSidedAttribute(JoinState &state, Attribute attr1, Attribute attr2,
                        AbstractValue lhs_val, AbstractValue rhs_val);

  /**
   * Join formulas.
   */
  static PulseFormula joinFormulas(const AbductiveDomain &lhs,
                                   const AbductiveDomain &rhs);
};

} // namespace pulse

#endif // CHECKER_PULSE_PULSEJOIN_H
