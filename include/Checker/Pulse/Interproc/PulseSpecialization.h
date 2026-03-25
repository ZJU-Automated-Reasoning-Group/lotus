#ifndef CHECKER_PULSE_PULSESPECIALIZATION_H
#define CHECKER_PULSE_PULSESPECIALIZATION_H

#include "Checker/Pulse/Domain/PulseDomain.h"
#include "Checker/Pulse/Interproc/PulseSummary.h"

#include <map>
#include <string>
#include <vector>

#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>

namespace pulse {

/**
 * HeapPath: represents a path through the heap (e.g., p->field->subfield)
 * Production-ready implementation aligned with Infer's Specialization.HeapPath
 */
class HeapPath {
public:
  enum class PathElement {
    Pvar,        // Starting from a program variable
    FieldAccess, // Field access: .field
    Dereference, // Pointer dereference: *
    ArrayIndex   // Array index: [i]
  };

  struct Element {
    PathElement kind;
    std::string field_name; // For FieldAccess
    AbstractValue index;    // For ArrayIndex

    Element(PathElement k) : kind(k) {}
    Element(PathElement k, const std::string &fname)
        : kind(k), field_name(fname) {}
    Element(PathElement k, AbstractValue idx) : kind(k), index(idx) {}

    bool operator<(const Element &other) const {
      if (kind != other.kind)
        return kind < other.kind;
      if (kind == PathElement::FieldAccess)
        return field_name < other.field_name;
      if (kind == PathElement::ArrayIndex)
        return index < other.index;
      return false;
    }

    bool operator==(const Element &other) const {
      if (kind != other.kind)
        return false;
      if (kind == PathElement::FieldAccess)
        return field_name == other.field_name;
      if (kind == PathElement::ArrayIndex)
        return index == other.index;
      return true;
    }
  };

private:
  std::vector<Element> path_;

public:
  HeapPath() = default;
  explicit HeapPath(const std::vector<Element> &p) : path_(p) {}

  void push(Element e) { path_.push_back(e); }
  const std::vector<Element> &getPath() const { return path_; }

  bool operator<(const HeapPath &other) const { return path_ < other.path_; }

  bool operator==(const HeapPath &other) const { return path_ == other.path_; }
};

/**
 * SpecializationKey: defines how a summary should be specialized
 * Production-ready implementation aligned with Infer's Specialization.Pulse
 */
struct SpecializationKey {
  // Map from heap paths to abstract values that need dynamic type
  // specialization
  std::map<HeapPath, AbstractValue> heap_paths_to_values;

  // Aliasing information: which arguments are aliased
  std::map<AbstractValue, std::set<AbstractValue>> aliasing_map;

  // Dynamic types: map from abstract values to their dynamic types
  std::map<AbstractValue, std::string> dynamic_types;

  bool operator<(const SpecializationKey &other) const {
    if (heap_paths_to_values != other.heap_paths_to_values) {
      return heap_paths_to_values < other.heap_paths_to_values;
    }
    if (aliasing_map != other.aliasing_map) {
      return aliasing_map < other.aliasing_map;
    }
    return dynamic_types < other.dynamic_types;
  }

  /**
   * Check if specialization is bottom (no specialization needed)
   */
  bool isBottom() const {
    return heap_paths_to_values.empty() && aliasing_map.empty() &&
           dynamic_types.empty();
  }
};

/**
 * SpecializationManager: manages specialized summaries
 * Production-ready implementation aligned with Infer's SpecializedCallGraph
 */
class SpecializationManager {
private:
  // Map: function -> specialization key -> specialized summary
  std::map<const llvm::Function *,
           std::map<SpecializationKey, std::unique_ptr<PulseSummary>>>
      specialized_summaries_;

  // Track which functions need specialization
  std::map<const llvm::Function *, std::set<SpecializationKey>>
      pending_specializations_;

public:
  SpecializationManager() = default;

  /**
   * Add a specialized summary
   */
  void addSpecializedSummary(const llvm::Function *func, SpecializationKey key,
                             std::unique_ptr<PulseSummary> summary) {
    specialized_summaries_[func][key] = std::move(summary);
    pending_specializations_[func].erase(key);
  }

  /**
   * Get specialized summary, or nullptr if not found
   */
  const PulseSummary *
  getSpecializedSummary(const llvm::Function *func,
                        const SpecializationKey &key) const {
    auto it = specialized_summaries_.find(func);
    if (it == specialized_summaries_.end())
      return nullptr;

    auto kit = it->second.find(key);
    if (kit == it->second.end())
      return nullptr;

    return kit->second.get();
  }

  /**
   * Check if a specialized summary exists
   */
  bool hasSpecializedSummary(const llvm::Function *func,
                             const SpecializationKey &key) const {
    return getSpecializedSummary(func, key) != nullptr;
  }

  /**
   * Request specialization for a function with given key
   */
  void requestSpecialization(const llvm::Function *func,
                             const SpecializationKey &key) {
    if (!key.isBottom()) {
      pending_specializations_[func].insert(key);
    }
  }

  /**
   * Get pending specializations for a function
   */
  const std::set<SpecializationKey> &
  getPendingSpecializations(const llvm::Function *func) const {
    static const std::set<SpecializationKey> empty;
    auto it = pending_specializations_.find(func);
    return (it != pending_specializations_.end()) ? it->second : empty;
  }

  /**
   * Compute specialization key from call site context
   */
  static SpecializationKey
  computeSpecializationKey(const AbductiveDomain &caller_state,
                           const llvm::CallInst *call,
                           const std::vector<AbstractValue> &actual_args);

  /**
   * Apply specialization to a domain (for specialized analysis)
   */
  static AbductiveDomain applySpecialization(const AbductiveDomain &astate,
                                             const SpecializationKey &key);
};

} // namespace pulse

#endif // CHECKER_PULSE_PULSESPECIALIZATION_H
