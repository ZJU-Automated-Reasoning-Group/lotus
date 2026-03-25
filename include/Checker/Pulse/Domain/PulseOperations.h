
#ifndef CHECKER_PULSE_PULSEOPERATIONS_H
#define CHECKER_PULSE_PULSEOPERATIONS_H

#include "Checker/Pulse/Core/PulseMemory.h"
#include "Checker/Pulse/Domain/PulseAbductiveDomain.h"
#include "Checker/Pulse/Domain/PulseExecutionDomain.h"

#include <atomic>
#include <functional>

#include <llvm/ADT/Optional.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Value.h>

// Forward declaration
namespace pulse {
class PulseFormula;
} // namespace pulse

namespace llvm {
class Value;
class Instruction;
class Function;
class BasicBlock;
} // namespace llvm

namespace pulse {

class AbstractValueFactory;

// OperationResult is defined in PulseExecutionDomain.h

struct OperationError {
  OperationResult kind;
  AbstractValue addr;
  const llvm::Instruction *location;
  std::string message;
};

/**
 * PulseOperations: high-level operations on the domain
 */
class PulseOperations {
private:
  AbstractValueFactory *factory_;

public:
  explicit PulseOperations(AbstractValueFactory *factory) : factory_(factory) {}

  /**
   * Evaluate an expression to an abstract address.
   * pred: predecessor block when resolving PHI incoming value (PHI handled in
   * checker).
   */
  llvm::Optional<Address> eval(AbductiveDomain &astate, const llvm::Value *exp,
                               const llvm::Instruction *loc,
                               const llvm::BasicBlock *pred = nullptr);

  /**
   * Evaluate a dereference: *ptr
   */
  std::pair<OperationResult, llvm::Optional<Address>>
  evalDeref(AbductiveDomain &astate, Address ptr, const llvm::Instruction *loc);

  /**
   * Check that an address is valid before access
   */
  OperationResult checkAddrAccess(AbductiveDomain &astate, Address addr,
                                  const llvm::Instruction *loc);

  /**
   * Allocate memory: mark address as allocated
   */
  void allocate(AbductiveDomain &astate, AbstractValue addr,
                const llvm::Instruction *loc);

  /**
   * Invalidate memory: mark as freed/out of scope.
   * \p kind is used for richer diagnostics (e.g. "freed at X (free)").
   */
  void invalidate(AbductiveDomain &astate, Address addr,
                  const llvm::Instruction *loc,
                  InvalidationKind kind = InvalidationKind::Other);

  /**
   * Write to memory: *ptr = value
   */
  OperationResult writeDeref(AbductiveDomain &astate, Address ptr,
                             Address value, const llvm::Instruction *loc);

  /**
   * Read from memory: value = *ptr
   */
  std::pair<OperationResult, llvm::Optional<Address>>
  readDeref(AbductiveDomain &astate, Address ptr, const llvm::Instruction *loc);

  /**
   * Initialize a variable
   */
  void initialize(AbductiveDomain &astate, AbstractValue addr);

  /**
   * Check for null pointer
   */
  OperationResult checkNull(AbductiveDomain &astate, Address addr,
                            const llvm::Instruction *loc);

  /**
   * Check if an Address originated from a null constant
   * Used by NPD checker to only report null dereferences from null constants
   */
  static bool isNullConstantSource(const Address &addr);

  /**
   * Shallow copy: create a new cell with the same edges as the original
   * Returns the address of the new cell
   */
  std::pair<OperationResult, llvm::Optional<Address>>
  shallowCopy(AbductiveDomain &astate, Address source,
              const llvm::Instruction *loc);

  /**
   * Deep copy: create a new cell with copied edges from the original
   * depth_max: maximum depth to copy (0 = unlimited)
   */
  std::pair<OperationResult, llvm::Optional<Address>>
  deepCopy(AbductiveDomain &astate, Address source,
           const llvm::Instruction *loc, unsigned depth_max = 0);

  /**
   * Havoc: model unknown effects by removing edges and attributes
   * Used for modeling unknown function calls
   */
  void havoc(AbductiveDomain &astate, Address addr,
             const llvm::Instruction *loc);

  /**
   * Check address escape: verify that a stack address doesn't escape
   * Returns error if address escapes (should be heap-allocated)
   */
  OperationResult checkAddressEscape(AbductiveDomain &astate, Address addr,
                                     const llvm::Function *current_function,
                                     const llvm::Instruction *loc);

private:
  /**
   * Helper for deep copy: recursively copy edges
   */
  void deepCopyRecursive(AbductiveDomain &astate, AbstractValue source,
                         AbstractValue target, unsigned depth_max,
                         unsigned current_depth);
};

/**
 * AbstractValueFactory: creates and manages abstract values.
 * When setMustAliasFn is provided, getOrCreate canonicalizes via must-alias.
 */
class AbstractValueFactory {
private:
  std::map<const llvm::Value *, AbstractValue> value_map_;
  std::function<bool(const llvm::Value *, const llvm::Value *)> mustAliasFn_;
  // Global ID generator to ensure uniqueness even if multiple factories exist
  // (e.g., join operations that create temporary factories).
  static std::atomic<unsigned> global_next_id_;

public:
  AbstractValueFactory() = default;

  void setMustAliasFn(
      std::function<bool(const llvm::Value *, const llvm::Value *)> fn) {
    mustAliasFn_ = std::move(fn);
  }

  AbstractValue getOrCreate(const llvm::Value *v);
  AbstractValue get(const llvm::Value *v) const;
  bool has(const llvm::Value *v) const;

  AbstractValue createFresh(const llvm::Value *hint = nullptr);
};

} // namespace pulse

#endif // CHECKER_PULSE_PULSEOPERATIONS_H
