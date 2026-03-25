#ifndef CHECKER_PULSE_PULSECALLSTATE_H
#define CHECKER_PULSE_PULSECALLSTATE_H

#include "Checker/Pulse/Core/PulseMemory.h"
#include "Checker/Pulse/Core/PulseSubstitution.h"
#include "Checker/Pulse/Core/PulseValueHistory.h"
#include "Checker/Pulse/Interproc/PulseSpecialization.h"

#include <map>
#include <set>
#include <string>
#include <vector>

#include <llvm/ADT/Optional.h>
#include <llvm/IR/Value.h>

// Forward declarations
namespace pulse {
class AbductiveDomain;
class PulseFormula;
} // namespace pulse

namespace pulse {

/**
 * CallState: sophisticated state tracking during summary application
 * Aligned with Infer's call_state structure in PulseInterproc.ml
 *
 * Tracks:
 * - Substitution from callee to caller addresses
 * - Reverse substitution with heap paths
 * - Per-cell history tracking
 * - Visited addresses
 * - Delayed array index handling
 * - Error tracking
 * - Alias tracking
 */
class CallState {
public:
  /**
   * CalleeIndexToVisit: delayed visit for array indices
   */
  struct CalleeIndexToVisit {
    AbstractValue addr_pre_dest; // Destination address in pre
    ValueHistory pre_hist;       // History in pre
    Access access_callee;        // Access in callee
    std::pair<AbstractValue, ValueHistory>
        addr_hist_caller; // Address/history in caller

    CalleeIndexToVisit(AbstractValue addr_pre, const ValueHistory &hist,
                       Access acc,
                       const std::pair<AbstractValue, ValueHistory> &caller)
        : addr_pre_dest(addr_pre), pre_hist(hist), access_callee(acc),
          addr_hist_caller(caller) {}
  };

  /**
   * LazyHeapPath: represents a heap path that may be unsupported
   */
  class LazyHeapPath {
  public:
    enum class Kind {
      Supported,  // Path is supported
      Unsupported // Path contains unsupported accesses
    };

  private:
    Kind kind_ = Kind::Unsupported;
    std::vector<Access> stack_;         // Reversed path (newest first)
    const llvm::Value *pvar_ = nullptr; // Starting program variable

  public:
    LazyHeapPath() = default;

    static LazyHeapPath fromPvar(const llvm::Value *pvar) {
      LazyHeapPath path;
      path.kind_ = Kind::Supported;
      path.pvar_ = pvar;
      Access deref(AccessKind::Dereference);
      path.stack_.push_back(deref);
      return path;
    }

    static LazyHeapPath unsupported() {
      LazyHeapPath path;
      path.kind_ = Kind::Unsupported;
      return path;
    }

    LazyHeapPath push(Access access) const {
      if (kind_ == Kind::Unsupported) {
        return unsupported();
      }
      LazyHeapPath new_path = *this;
      new_path.stack_.push_back(access);
      return new_path;
    }

    llvm::Optional<HeapPath> force() const {
      if (kind_ == Kind::Unsupported || !pvar_) {
        return llvm::None;
      }
      std::vector<HeapPath::Element> path_elements;
      path_elements.push_back(HeapPath::Element(HeapPath::PathElement::Pvar));
      for (const Access &acc : stack_) {
        switch (acc.kind) {
        case AccessKind::Dereference:
          path_elements.push_back(
              HeapPath::Element(HeapPath::PathElement::Dereference));
          break;
        case AccessKind::Field:
          path_elements.push_back(
              HeapPath::Element(HeapPath::PathElement::FieldAccess,
                                "f" + std::to_string(acc.field_idx)));
          break;
        case AccessKind::ArrayIndex:
          // Preserve the symbolic index value. The physical stride
          // (if any) is part of `Access` identity for heap modeling,
          // but does not need to be rendered in the heap-path UI.
          path_elements.push_back(
              HeapPath::Element(HeapPath::PathElement::ArrayIndex, acc.index));
          break;
        }
      }
      return HeapPath(path_elements);
    }

    bool isSupported() const { return kind_ == Kind::Supported; }
  };

private:
  // Caller's abstract state computed so far
  std::unique_ptr<AbductiveDomain> astate_;

  // Translation from callee addresses to caller addresses and their caller
  // histories
  Substitution subst_;

  // Reverse translation: caller addresses -> (callee address, heap path)
  std::map<AbstractValue, std::pair<AbstractValue, LazyHeapPath>> rev_subst_;

  // Per-cell history tracking: maps CellId to caller history
  // Simplified: use AbstractValue as cell identifier
  std::map<AbstractValue, ValueHistory> hist_map_;

  // Set of callee addresses that have been visited already
  std::set<AbstractValue> visited_;

  // Delayed visit for array indices
  std::vector<CalleeIndexToVisit> array_indices_to_visit_;

  // First error during materialization
  llvm::Optional<AbstractValue> first_error_;

  // Alias tracking: if alias detected between addr_callee and addr_callee' with
  // paths, aliases[addr_callee] contains addr_callee' Simplified: map from
  // AbstractValue to set of AbstractValues
  std::map<AbstractValue, std::set<AbstractValue>> aliases_;

public:
  CallState(std::unique_ptr<AbductiveDomain> astate)
      : astate_(std::move(astate)) {}

  // Accessors
  AbductiveDomain &getAstate() { return *astate_; }
  const AbductiveDomain &getAstate() const { return *astate_; }

  Substitution &getSubst() { return subst_; }
  const Substitution &getSubst() const { return subst_; }

  const std::map<AbstractValue, std::pair<AbstractValue, LazyHeapPath>> &
  getRevSubst() const {
    return rev_subst_;
  }

  const std::map<AbstractValue, ValueHistory> &getHistMap() const {
    return hist_map_;
  }
  std::map<AbstractValue, ValueHistory> &getHistMap() { return hist_map_; }

  const std::set<AbstractValue> &getVisited() const { return visited_; }
  std::set<AbstractValue> &getVisited() { return visited_; }

  const std::vector<CalleeIndexToVisit> &getArrayIndicesToVisit() const {
    return array_indices_to_visit_;
  }
  std::vector<CalleeIndexToVisit> &getArrayIndicesToVisit() {
    return array_indices_to_visit_;
  }

  llvm::Optional<AbstractValue> getFirstError() const { return first_error_; }
  void setFirstError(AbstractValue addr) {
    if (!first_error_) {
      first_error_ = addr;
    }
  }

  const std::map<AbstractValue, std::set<AbstractValue>> &getAliases() const {
    return aliases_;
  }
  std::map<AbstractValue, std::set<AbstractValue>> &getAliases() {
    return aliases_;
  }

  /**
   * Add alias: record that addr_callee and addr_callee' are aliased
   */
  void addAlias(AbstractValue addr_callee, AbstractValue addr_callee_prime) {
    aliases_[addr_callee].insert(addr_callee_prime);
    aliases_[addr_callee_prime].insert(addr_callee);
  }

  /**
   * Add to reverse substitution
   */
  void addRevSubst(AbstractValue addr_caller, AbstractValue addr_callee,
                   const LazyHeapPath &path) {
    rev_subst_[addr_caller] = {addr_callee, path};
  }

  /**
   * Find callee address for caller address
   */
  llvm::Optional<std::pair<AbstractValue, LazyHeapPath>>
  toCalleeAddr(AbstractValue addr_caller) const {
    auto it = rev_subst_.find(addr_caller);
    if (it != rev_subst_.end()) {
      return it->second;
    }
    return llvm::None;
  }

  /**
   * Find caller value for callee address (with normalization)
   */
  llvm::Optional<std::pair<AbstractValue, ValueHistory>>
  toCallerValue(AbstractValue addr_callee) const {
    auto caller_opt = subst_.substitute(addr_callee);
    if (!caller_opt) {
      return llvm::None;
    }
    // Normalize caller value
    AbstractValue caller_canon = astate_->getCanonical(*caller_opt);

    // Get history from hist_map if available, otherwise use epoch
    ValueHistory hist;
    auto hist_it = hist_map_.find(addr_callee);
    if (hist_it != hist_map_.end()) {
      hist = hist_it->second;
    }

    return std::make_pair(caller_canon, hist);
  }

  /**
   * Incorporate new equalities discovered during materialization
   * Updates both astate and substitution maps
   */
  bool incorporateNewEqs(const PulseFormula &new_eqs);
};

} // namespace pulse

#endif // CHECKER_PULSE_PULSECALLSTATE_H
