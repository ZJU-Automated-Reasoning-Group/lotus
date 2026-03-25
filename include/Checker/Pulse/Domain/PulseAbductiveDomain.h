#ifndef CHECKER_PULSE_PULSEABDUCTIVEDOMAIN_H
#define CHECKER_PULSE_PULSEABDUCTIVEDOMAIN_H

#include "Checker/Pulse/Core/PulseFormula.h"
#include "Checker/Pulse/Core/PulseMemory.h"
#include "Checker/Pulse/Domain/PulseInvalidation.h"
#include "Checker/Pulse/Domain/PulseTaint.h"
#include "Checker/Pulse/Interproc/PulseTransitiveInfo.h"

#include <cstdint>
#include <map>
#include <memory>
#include <set>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Instruction.h>

// Forward declaration to avoid circular dependency
namespace pulse {
class HeapPath;
} // namespace pulse

namespace pulse {

/**
 * AbductiveDomain: the core abstract domain
 * Maintains:
 * - Post state: current abstract state
 * - Pre state: inferred pre-condition (biabduction)
 * - Path condition: constraints via PulseFormula
 *
 * Infer Pulse-inspired semantics (sound incorrectness):
 * - The post-state represents what is known to hold along the current witness
 *   path.
 * - The pre-state records *missing* heap edges/attributes that were required by
 *   operations (reads/dereferences) but not present in the current state. This
 *   precondition is later materialized at call sites when applying summaries.
 * - Joining two states at a program point should approximate a disjunction of
 *   path conditions (keep only facts stable across both branches). Conjoining
 *   conditions would drop feasible witnesses (false negatives).
 *
 * This implementation is intentionally conservative in what it proves: when it
 * cannot establish a property, it prefers to keep it unknown rather than guess.
 */
class AbductiveDomain {
private:
  Stack post_stack_;
  Heap post_heap_;
  AddressAttributes post_attrs_;

  Stack pre_stack_;
  Heap pre_heap_;
  AddressAttributes pre_attrs_;

  // Taint tracking
  TaintDomain taint_domain_;

  // Path condition: formula tracking equalities and constraints
  std::unique_ptr<PulseFormula> path_formula_;

  // Invalidation kind + location per invalid address (for richer diagnostics)
  std::map<AbstractValue,
           std::pair<InvalidationKind, const llvm::Instruction *>>
      invalidation_info_;

  // Allocation sizes (in bytes) for base addresses when known.
  std::map<AbstractValue, uint64_t> allocation_sizes_;

  // Skipped calls (for unknown functions)
  std::set<std::string> skipped_calls_;

  // Additional fields aligned with Infer's AbductiveDomain
  // TransitiveInfo: interprocedural information
  TransitiveInfo transitive_info_;

  // Unknown values flag: did we generate at least one unknown abstract value on
  // this path?
  bool unknown_values_ = false;

  // Need dynamic type specialization: set of abstract values that need
  // specialization
  std::set<AbstractValue> need_dynamic_type_specialization_;

  // Recursive calls: set of mutually recursive calls (simplified: just function
  // names)
  std::set<std::string> recursive_calls_;

  // Loop header info: map from loop header BB to iteration info
  // Simplified structure: just tracks which loop headers we've seen
  std::map<const llvm::BasicBlock *, unsigned> loop_header_info_;

  // Loop invariant under inference: optional (header BB, entry state)
  struct LoopInvariantUnderInference {
    const llvm::BasicBlock *header;
    std::unique_ptr<AbductiveDomain> entry_astate;

    LoopInvariantUnderInference(const llvm::BasicBlock *h,
                                AbductiveDomain astate)
        : header(h),
          entry_astate(std::make_unique<AbductiveDomain>(std::move(astate))) {}
  };
  std::unique_ptr<LoopInvariantUnderInference> loop_invariant_under_inference_;

public:
  // Stack operations
  Stack &getPostStack() { return post_stack_; }
  Stack &getPreStack() { return pre_stack_; }
  const Stack &getPostStack() const { return post_stack_; }
  const Stack &getPreStack() const { return pre_stack_; }

  // Heap operations
  Heap &getPostHeap() { return post_heap_; }
  Heap &getPreHeap() { return pre_heap_; }
  const Heap &getPostHeap() const { return post_heap_; }
  const Heap &getPreHeap() const { return pre_heap_; }

  // Attribute operations
  AddressAttributes &getPostAttrs() { return post_attrs_; }
  AddressAttributes &getPreAttrs() { return pre_attrs_; }
  const AddressAttributes &getPostAttrs() const { return post_attrs_; }
  const AddressAttributes &getPreAttrs() const { return pre_attrs_; }

  // Taint operations
  TaintDomain &getTaintDomain() { return taint_domain_; }
  const TaintDomain &getTaintDomain() const { return taint_domain_; }

  // Path condition
  PulseFormula &getPathFormula();
  const PulseFormula &getPathFormula() const;
  void setPathFormula(std::unique_ptr<PulseFormula> formula);

  // Convenience methods for path conditions
  void addNonNull(AbstractValue addr);
  bool isNonNull(AbstractValue addr) const;
  void addEquality(AbstractValue v1, AbstractValue v2);
  AbstractValue getCanonical(AbstractValue v) const;

  // Abduction: when reading from memory not in pre, add it to pre
  void abduceToPre(AbstractValue addr, const Access &access, Address target);
  void abduceAttrToPre(AbstractValue addr, Attribute attr);

  // Invalidation info (for UseAfterFree diagnostics)
  void setInvalidationInfo(AbstractValue addr, InvalidationKind kind,
                           const llvm::Instruction *loc);
  llvm::Optional<std::pair<InvalidationKind, const llvm::Instruction *>>
  getInvalidationInfo(AbstractValue addr) const;

  // Allocation size tracking
  void setAllocationSize(AbstractValue addr, uint64_t size_bytes) {
    if (size_bytes == 0) {
      allocation_sizes_.erase(addr);
    } else {
      allocation_sizes_[addr] = size_bytes;
    }
  }
  llvm::Optional<uint64_t> getAllocationSize(AbstractValue addr) const {
    auto it = allocation_sizes_.find(addr);
    if (it != allocation_sizes_.end()) {
      return it->second;
    }
    return llvm::None;
  }
  const std::map<AbstractValue, uint64_t> &getAllocationSizes() const {
    return allocation_sizes_;
  }
  std::map<AbstractValue, uint64_t> &getAllocationSizes() {
    return allocation_sizes_;
  }

  // Skipped calls
  void addSkippedCall(const std::string &name) { skipped_calls_.insert(name); }
  const std::set<std::string> &getSkippedCalls() const {
    return skipped_calls_;
  }
  bool hasSkippedCall(const std::string &name) const {
    return skipped_calls_.count(name) > 0;
  }

  // TransitiveInfo operations
  TransitiveInfo &getTransitiveInfo() { return transitive_info_; }
  const TransitiveInfo &getTransitiveInfo() const { return transitive_info_; }
  void setTransitiveInfo(const TransitiveInfo &info) {
    transitive_info_ = info;
  }

  // Unknown values flag
  bool hasUnknownValues() const { return unknown_values_; }
  void setUnknownValues(bool val) { unknown_values_ = val; }
  void declareUnknownValues() { unknown_values_ = true; }

  // Dynamic type specialization
  const std::set<AbstractValue> &getNeedDynamicTypeSpecialization() const {
    return need_dynamic_type_specialization_;
  }
  void addNeedDynamicTypeSpecialization(AbstractValue av) {
    need_dynamic_type_specialization_.insert(av);
  }
  void clearNeedDynamicTypeSpecialization() {
    need_dynamic_type_specialization_.clear();
  }

  // Recursive calls
  const std::set<std::string> &getRecursiveCalls() const {
    return recursive_calls_;
  }
  void addRecursiveCall(const std::string &name) {
    recursive_calls_.insert(name);
  }
  void addRecursiveCalls(const std::set<std::string> &calls) {
    recursive_calls_.insert(calls.begin(), calls.end());
  }

  // Loop header info
  const std::map<const llvm::BasicBlock *, unsigned> &
  getLoopHeaderInfo() const {
    return loop_header_info_;
  }
  void initLoopHeaderInfo(const llvm::BasicBlock *header) {
    loop_header_info_[header] = 0;
  }
  void removeLoopHeaderInfo(const llvm::BasicBlock *header) {
    loop_header_info_.erase(header);
  }
  void pushLoopHeaderInfo(const llvm::BasicBlock *header, unsigned timestamp) {
    loop_header_info_[header] = timestamp;
  }

  // Loop invariant under inference
  bool isLoopInvariantUnderInference(const llvm::BasicBlock *header) const {
    return loop_invariant_under_inference_ &&
           loop_invariant_under_inference_->header == header;
  }
  bool isSomeLoopInvariantUnderInference() const {
    return loop_invariant_under_inference_ != nullptr;
  }
  void startLoopInvariantInference(const llvm::BasicBlock *header,
                                   AbductiveDomain entry_astate) {
    loop_invariant_under_inference_ =
        std::make_unique<LoopInvariantUnderInference>(header,
                                                      std::move(entry_astate));
  }
  void clearLoopInvariantInference() {
    loop_invariant_under_inference_.reset();
  }

  // Constructors and assignment operators
  AbductiveDomain() : path_formula_(nullptr) {}
  ~AbductiveDomain();

  // Delete copy operations (use clone() instead)
  AbductiveDomain(const AbductiveDomain &) = delete;
  AbductiveDomain &operator=(const AbductiveDomain &) = delete;

  // Move operations
  AbductiveDomain(AbductiveDomain &&) = default;
  AbductiveDomain &operator=(AbductiveDomain &&) = default;

  // Clone domain
  AbductiveDomain clone() const;

  // Normalize internal maps (stack/heap/attrs) w.r.t. current path formula.
  // This helps keep heap keys consistent under aliasing/equality.
  void canonicalize();

  // Merge two domains (for joining exit states)
  // Returns merged domain or empty if merge is impossible
  static llvm::Optional<AbductiveDomain> merge(const AbductiveDomain &d1,
                                               const AbductiveDomain &d2);
};

} // namespace pulse

#endif // CHECKER_PULSE_PULSEABDUCTIVEDOMAIN_H
