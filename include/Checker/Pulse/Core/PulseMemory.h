#ifndef CHECKER_PULSE_PULSEMEMORY_H
#define CHECKER_PULSE_PULSEMEMORY_H

#include "Checker/Pulse/Core/PulseAbstractValue.h"
#include "Checker/Pulse/Core/PulseValueHistory.h"

#include <cstdint>
#include <map>
#include <set>

#include <llvm/IR/Value.h>

namespace pulse {

/**
 * Address: abstract address with history (for error reporting)
 */
struct Address {
  AbstractValue addr;
  ValueHistory history; // How we got here

  Address() : addr() {}
  Address(AbstractValue a) : addr(a) {}
};

/**
 * Memory attributes: properties attached to addresses
 */
enum class Attribute {
  Allocated,     // Memory was allocated
  Stack,         // Address is derived from stack allocation (alloca)
  Global,        // Address is a global variable / global storage
  Invalid,       // Memory is invalid (freed, out of scope)
  Uninitialized, // Memory is uninitialized
  Null,          // Pointer is null
  OutOfBounds,   // Pointer proven to be out of bounds for its allocation
  Tainted,       // Value is tainted
  FileHandle,    // File handle resource
  Lock,          // Lock resource
  AsyncResource  // Async/awaitable resource
};

using AttributeSet = std::set<Attribute>;

/**
 * Access path: field access, array index, or dereference
 */
enum class AccessKind { Dereference, Field, ArrayIndex };

/**
 * Access: one step in an access path keying heap edges.
 *
 * Design notes (sound incorrectness / Pulse-style):
 * - We must not conflate different memory projections. In particular, GEP over
 *   struct fields is not the same as pointer/array indexing, even if the
 *   index operand is a constant 0/1/2.
 * - Array indexing may use a symbolic index; to keep distinct projections from
 *   accidentally collapsing, we include a best-effort byte stride when known
 *   (via LLVM DataLayout). If stride is unknown, it is left as 0.
 *
 * This structure is used as a `std::map` key inside `Heap`, so ordering must be
 * stable and deterministic.
 */
struct Access {
  AccessKind kind;
  // For Field
  unsigned field_idx{0};
  // For ArrayIndex
  AbstractValue index{};
  // Size in bytes of the indexed element (0 if unknown / not applicable).
  uint64_t stride_bytes{0};

  Access() : kind(AccessKind::Dereference) {}
  explicit Access(AccessKind k) : kind(k) {}
  Access(unsigned idx) : kind(AccessKind::Field), field_idx(idx) {}

  static Access arrayIndex(AbstractValue idx, uint64_t stride_bytes = 0) {
    Access a;
    a.kind = AccessKind::ArrayIndex;
    a.index = idx;
    a.stride_bytes = stride_bytes;
    return a;
  }

  bool operator<(const Access &other) const {
    if (kind != other.kind)
      return kind < other.kind;
    if (kind == AccessKind::Field)
      return field_idx < other.field_idx;
    if (kind == AccessKind::ArrayIndex) {
      // Stride is part of the projection identity: same index value but
      // different element types should not necessarily alias.
      if (stride_bytes != other.stride_bytes)
        return stride_bytes < other.stride_bytes;
      return index < other.index;
    }
    return false;
  }
};

/**
 * Stack: maps variables to abstract addresses
 */
class Stack {
private:
  std::map<const llvm::Value *, Address> stack_;

public:
  // Allow access for merging
  const std::map<const llvm::Value *, Address> &getMap() const {
    return stack_;
  }
  void add(const llvm::Value *var, Address addr);
  Address *find(const llvm::Value *var);
  const Address *find(const llvm::Value *var) const;
  void remove(const llvm::Value *var);
  void clear();
};

/**
 * Heap: graph of abstract addresses connected by access paths
 */
class Heap {
private:
  // addr -> (access -> target_addr)
  std::map<AbstractValue, std::map<Access, Address>> edges_;

public:
  // Allow access for merging
  const std::map<AbstractValue, std::map<Access, Address>> &getEdges() const {
    return edges_;
  }
  std::map<AbstractValue, std::map<Access, Address>> &getEdges() {
    return edges_;
  }
  void addEdge(AbstractValue from, Access access, Address to);
  Address *findEdge(AbstractValue from, Access access);
  const Address *findEdge(AbstractValue from, Access access) const;
  void removeEdges(AbstractValue addr);
};

/**
 * AddressAttributes: properties attached to addresses
 */
class AddressAttributes {
private:
  std::map<AbstractValue, AttributeSet> attrs_;

public:
  // Allow access for merging
  const std::map<AbstractValue, AttributeSet> &getAttrs() const {
    return attrs_;
  }
  std::map<AbstractValue, AttributeSet> &getAttrs() { return attrs_; }
  void add(AbstractValue addr, Attribute attr);
  void remove(AbstractValue addr, Attribute attr);
  bool has(AbstractValue addr, Attribute attr) const;
  AttributeSet get(AbstractValue addr) const;
  void clear(AbstractValue addr);
};

} // namespace pulse

#endif // CHECKER_PULSE_PULSEMEMORY_H
