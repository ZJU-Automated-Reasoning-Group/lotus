/**
 * @file PtsSet.h
 * @brief Sparse-bit-vector points-to set for Andersen's analysis.
 *
 * ## Design
 *
 * `AndersPtsSet` is the **concrete** points-to set implementation backed by
 * `llvm::SparseBitVector`.  Each element of the set is a `NodeIndex` (an
 * unsigned integer) identifying a memory object in the constraint graph.
 *
 * The sparse bit-vector representation is efficient when the points-to sets
 * are sparse (few objects per pointer), which is the common case.  For
 * programs with very large or dense points-to sets, the BDD-backed
 * `BDDAndersPtsSet` (see `BDDPtsSet.h`) may be more efficient.
 *
 * ## Abstraction Boundary
 *
 * This class is intentionally isolated so that the backing data structure
 * can be swapped without changing the rest of the analysis.  The
 * `TemplatePtsSet.h` header provides a `RuntimePtsSet` wrapper that selects
 * between `AndersPtsSet` and `BDDAndersPtsSet` at runtime.
 *
 * @note `getSize()` is **not** O(1) — it counts set bits in the sparse
 *       vector.  Prefer `isEmpty()` for empty checks.
 */

#ifndef ANDERSEN_PTSSET_H
#define ANDERSEN_PTSSET_H

#include <llvm/ADT/SparseBitVector.h>

/**
 * @class AndersPtsSet
 * @brief Points-to set backed by `llvm::SparseBitVector`.
 *
 * Elements are `NodeIndex` values (unsigned integers).  All set operations
 * return `true` if the set was modified (changed), `false` otherwise —
 * this convention is used by the constraint solver to detect fixed-point.
 */
class AndersPtsSet {
private:
  llvm::SparseBitVector<> bitvec; ///< Underlying sparse bit-vector storage.

public:
  using iterator = llvm::SparseBitVector<>::iterator;

  /**
   * @brief Return `true` if @p idx is in this set.
   *
   * @note The non-const overload exists because `SparseBitVector::test()`
   *       is not marked `const` in LLVM.  The const overload uses a
   *       workaround via `contains()`.
   */
  bool has(unsigned idx) { return bitvec.test(idx); }
  bool has(unsigned idx) const {
    // SparseBitVector::test() lacks a const qualifier, so we use contains()
    // with a single-element vector as a workaround.
    llvm::SparseBitVector<> idVec;
    idVec.set(idx);
    return bitvec.contains(idVec);
  }

  /// @brief Insert @p idx into the set.  @return `true` if the set changed.
  bool insert(unsigned idx) { return bitvec.test_and_set(idx); }

  /// @brief Return `true` if this set is a superset of @p other.
  bool contains(const AndersPtsSet &other) const {
    return bitvec.contains(other.bitvec);
  }

  /// @brief Return `true` if this set and @p other share at least one element.
  bool intersectWith(const AndersPtsSet &other) const {
    return bitvec.intersects(other.bitvec);
  }

  /// @brief Union @p other into this set.  @return `true` if the set changed.
  bool unionWith(const AndersPtsSet &other) { return bitvec |= other.bitvec; }

  /// @brief Remove all elements from the set.
  void clear() { bitvec.clear(); }

  /// @brief Return the number of elements.
  /// @warning This is **not** O(1) — it counts set bits.  Use `isEmpty()` for
  ///          empty checks.
  unsigned getSize() const { return bitvec.count(); }

  /// @brief Return `true` if the set is empty.  Prefer this over
  /// `getSize()==0`.
  bool isEmpty() const { return bitvec.empty(); }

  bool operator==(const AndersPtsSet &other) const {
    return bitvec == other.bitvec;
  }

  iterator begin() const { return bitvec.begin(); }
  iterator end() const { return bitvec.end(); }
};

#endif
