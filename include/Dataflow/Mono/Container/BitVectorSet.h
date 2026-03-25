#ifndef LOTUS_DATAFLOW_MONO_CONTAINER_BITVECTORSET_H_
#define LOTUS_DATAFLOW_MONO_CONTAINER_BITVECTORSET_H_

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Value.h"

#include <algorithm>
#include <iterator>
#include <vector>

namespace mono {

/**
 * @brief A set implementation backed by a bit-vector for efficient dataflow
 * analysis
 *
 * This class provides a set-like interface optimized for monotone dataflow
 * analyses where the universe of elements is fixed and known in advance.
 * Operations like union, intersection, and equality checks are O(N/64) where N
 * is the universe size.
 *
 * **Performance characteristics:**
 * - Union/Intersection/Difference: O(N/64) bit operations
 * - Equality check: O(N/64) comparison
 * - Insert/Contains: O(1) bit operations
 * - Space: N/8 bytes (plus universe storage)
 *
 * **When to use:**
 * - Large universe (>100 elements) with frequent set operations
 * - Many temporary set allocations during analysis
 * - Performance-critical analyses (e.g., reaching definitions, available
 * expressions)
 *
 * **When NOT to use:**
 * - Small universe (<20 elements) - std::set is faster due to cache locality
 * - Dynamic universe that grows during analysis
 * - Sparse sets with very few elements relative to universe
 *
 * @tparam T The element type (must be hashable)
 */
template <typename T> class BitVectorSet {
public:
  /**
   * @brief Constructs an empty bit-vector set with no universe
   *
   * Note: You must call setUniverse() before using insert/contains operations
   */
  BitVectorSet() = default;

  /**
   * @brief Constructs a bit-vector set with the given universe
   *
   * @param Universe The complete set of possible elements
   */
  explicit BitVectorSet(const std::vector<T> &Universe) {
    setUniverse(Universe);
  }

  /**
   * @brief Sets the universe for this bit-vector set
   *
   * This must be called before any insert/contains operations. All
   * BitVectorSets in a dataflow analysis should share the same universe for
   * correctness.
   *
   * @param Universe The complete set of possible elements
   */
  void setUniverse(const std::vector<T> &Universe) {
    ElementToIndex.clear();
    IndexToElement.clear();
    for (size_t I = 0; I < Universe.size(); ++I) {
      ElementToIndex[Universe[I]] = I;
      IndexToElement.push_back(Universe[I]);
    }
    Bits.resize(Universe.size());
  }

  /**
   * @brief Inserts an element into the set
   *
   * @param Elem The element to insert (must be in the universe)
   * @return true if the element was inserted, false if already present
   */
  bool insert(const T &Elem) {
    auto It = ElementToIndex.find(Elem);
    if (It == ElementToIndex.end()) {
      // Element not in universe - silently ignore (matches std::set behavior
      // for invalid elements in a bounded domain)
      return false;
    }
    size_t Idx = It->second;
    bool WasPresent = Bits.test(Idx);
    Bits.set(Idx);
    return !WasPresent;
  }

  /**
   * @brief Removes an element from the set
   *
   * @param Elem The element to remove
   * @return true if the element was removed, false if not present
   */
  bool erase(const T &Elem) {
    auto It = ElementToIndex.find(Elem);
    if (It == ElementToIndex.end()) {
      return false;
    }
    size_t Idx = It->second;
    bool WasPresent = Bits.test(Idx);
    Bits.reset(Idx);
    return WasPresent;
  }

  /**
   * @brief Checks if an element is in the set
   *
   * @param Elem The element to check
   * @return true if the element is present
   */
  bool contains(const T &Elem) const {
    auto It = ElementToIndex.find(Elem);
    if (It == ElementToIndex.end()) {
      return false;
    }
    return Bits.test(It->second);
  }

  /**
   * @brief Returns the number of elements in the set
   */
  size_t size() const { return Bits.count(); }

  /**
   * @brief Checks if the set is empty
   */
  bool empty() const { return Bits.none(); }

  /**
   * @brief Removes all elements from the set
   */
  void clear() { Bits.reset(); }

  /**
   * @brief Set union: this = this ∪ other
   *
   * Assert that both sets share the same universe size before
   * performing bit-level operations.  Mismatched universes would cause
   * llvm::BitVector to silently truncate to the shorter length, producing
   * incorrect results.
   *
   * @param Other The set to union with
   */
  void unionWith(const BitVectorSet &Other) {
    assert(Bits.size() == Other.Bits.size() &&
           "BitVectorSet::unionWith: universe size mismatch — both sets must "
           "be initialized with the same universe");
    Bits |= Other.Bits;
  }

  /**
   * @brief Set intersection: this = this ∩ other
   *
   * assert matching universe sizes.
   *
   * @param Other The set to intersect with
   */
  void intersectWith(const BitVectorSet &Other) {
    assert(Bits.size() == Other.Bits.size() &&
           "BitVectorSet::intersectWith: universe size mismatch — both sets "
           "must be initialized with the same universe");
    Bits &= Other.Bits;
  }

  /**
   * @brief Set difference: this = this - other
   *
   *
   * @param Other The set to subtract
   */
  void differenceWith(const BitVectorSet &Other) {
    assert(Bits.size() == Other.Bits.size() &&
           "BitVectorSet::differenceWith: universe size mismatch — both sets "
           "must be initialized with the same universe");
    Bits.reset(Other.Bits);
  }

  /**
   * @brief Equality comparison
   */
  bool operator==(const BitVectorSet &Other) const {
    return Bits == Other.Bits;
  }

  bool operator!=(const BitVectorSet &Other) const { return !(*this == Other); }

  /**
   * @brief Iterator support for range-based for loops
   *
   * Example:
   *   for (auto *Inst : LiveSet) {
   *     // Process instruction...
   *   }
   */
  class iterator {
  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = const T *;
    using reference = const T &;

    iterator(const BitVectorSet *Parent, int Idx)
        : Parent(Parent), CurrentIdx(Idx) {
      advance();
    }

    reference operator*() const { return Parent->IndexToElement[CurrentIdx]; }

    pointer operator->() const { return &Parent->IndexToElement[CurrentIdx]; }

    iterator &operator++() {
      ++CurrentIdx;
      advance();
      return *this;
    }

    iterator operator++(int) {
      iterator Tmp = *this;
      ++(*this);
      return Tmp;
    }

    bool operator==(const iterator &Other) const {
      return CurrentIdx == Other.CurrentIdx;
    }

    bool operator!=(const iterator &Other) const { return !(*this == Other); }

  private:
    void advance() {
      while (CurrentIdx < static_cast<int>(Parent->Bits.size()) &&
             !Parent->Bits.test(CurrentIdx)) {
        ++CurrentIdx;
      }
    }

    const BitVectorSet *Parent;
    int CurrentIdx;
  };

  iterator begin() const { return iterator(this, 0); }

  iterator end() const { return iterator(this, Bits.size()); }

  /**
   * @brief Returns the underlying bit-vector (for advanced use cases)
   */
  const llvm::BitVector &getBits() const { return Bits; }

  /**
   * @brief Returns the universe size
   */
  size_t universeSize() const { return IndexToElement.size(); }

private:
  llvm::BitVector Bits;
  llvm::DenseMap<T, size_t> ElementToIndex;
  std::vector<T> IndexToElement;
};

/**
 * @brief Helper function to create a universe from all instructions in a
 * function
 *
 * This is useful for instruction-based dataflow analyses (reaching definitions,
 * available expressions, etc.)
 *
 * @param F The function
 * @return A vector of all instructions in the function
 */
inline std::vector<llvm::Instruction *> getAllInstructions(llvm::Function *F) {
  std::vector<llvm::Instruction *> Result;
  if (F == nullptr) {
    return Result;
  }
  for (auto &BB : *F) {
    for (auto &Inst : BB) {
      Result.push_back(&Inst);
    }
  }
  return Result;
}

/**
 * @brief Helper function to create a universe from all SSA values in a function
 *
 * This includes all instructions that produce values plus function arguments.
 *
 * @param F The function
 * @return A vector of all SSA values (instructions + arguments)
 */
inline std::vector<llvm::Value *> getAllSSAValues(llvm::Function *F) {
  std::vector<llvm::Value *> Result;
  if (F == nullptr) {
    return Result;
  }
  for (auto &Arg : F->args()) {
    Result.push_back(&Arg);
  }
  for (auto &BB : *F) {
    for (auto &Inst : BB) {
      if (!Inst.getType()->isVoidTy()) {
        Result.push_back(&Inst);
      }
    }
  }
  return Result;
}

} // namespace mono

#endif // LOTUS_DATAFLOW_MONO_CONTAINER_BITVECTORSET_H_
