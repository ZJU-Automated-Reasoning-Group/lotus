//===----------------------------------------------------------------------===//
/// @file Offset.h
/// @brief Offset wrapper type for handling file/memory offsets
///
/// This file defines the Offset struct, a wrapper around uint64_t that provides
/// special handling for unknown offsets. The UNKNOWN constant represents an
/// indeterminate or unspecified offset value.
///
/// The Offset type supports:
/// - Arithmetic operations (addition, subtraction, bitwise NOT)
/// - Comparison operations with strict semantics
/// - Range checking
/// - Stream output for debugging
///
///===----------------------------------------------------------------------===//

#ifndef OFFSET_H_
#define OFFSET_H_

#include <cstdint>

#ifndef NDEBUG
#include <iostream>
#endif // not NDEBUG

// just a wrapper around uint64_t to
// handle Offset::UNKNOWN somehow easily
// maybe later we'll make it a range
/// @brief Wrapper type for representing offsets with unknown value support
///
/// This struct wraps a uint64_t and provides special semantics for an unknown
/// offset value. Arithmetic operations propagate the unknown state, and
/// comparison operations maintain strict semantics.
struct Offset {
  /// @brief The underlying type for the offset value
  using type = uint64_t;

  /// @brief Special constant representing an unknown/undefined offset
  static const type UNKNOWN;

  /// @brief Get an Offset representing the unknown value
  /// @return An Offset initialized to UNKNOWN
  static Offset getUnknown() { return Offset(Offset::UNKNOWN); }

  /// @brief Get an Offset representing zero
  /// @return An Offset initialized to 0
  static Offset getZero() { return Offset(0); }

  // cast to type
  // operator type() { return offset; }

  /// @brief Default constructor
  /// @param o The offset value (defaults to UNKNOWN)
  Offset(type o = UNKNOWN) : offset(o) {}
  Offset(const Offset &) = default;

  /// @brief Addition operator
  /// @param o The offset to add
  /// @return A new Offset with the sum, or UNKNOWN if either is unknown
  Offset operator+(const Offset o) const {
    if (offset == UNKNOWN || o.offset == UNKNOWN ||
        offset >= UNKNOWN - o.offset) {
      return UNKNOWN;
    }

    return Offset(offset + o.offset);
  }

  /// @brief Addition assignment operator
  /// @param o The offset to add
  /// @return Reference to this Offset
  Offset &operator+=(const Offset o) {
    if (offset == UNKNOWN || o.offset == UNKNOWN ||
        offset >= UNKNOWN - o.offset) {
      offset = UNKNOWN;
    } else {
      offset += o.offset;
    }

    return *this;
  }

  /// @brief Assignment operator
  /// @param o The offset to assign
  /// @return Reference to this Offset
  Offset &operator=(const Offset o) {
    offset = o.offset;
    return *this;
  }

  /// @brief Subtraction operator
  /// @param o The offset to subtract
  /// @return A new Offset with the difference, or UNKNOWN if either is unknown
  Offset operator-(const Offset &o) const {
    if (offset == UNKNOWN || o.offset == UNKNOWN || offset < o.offset) {
      return Offset(UNKNOWN);
    }

    return Offset(offset - o.offset);
  }

  /// @brief Subtraction assignment operator
  /// @param o The offset to subtract
  /// @return Reference to this Offset
  Offset &operator-(const Offset &o) {
    if (offset == UNKNOWN || o.offset == UNKNOWN || offset < o.offset) {
      offset = UNKNOWN;
    } else {
      offset -= o.offset;
    }
    return *this;
  }

  /// @brief Bitwise NOT operator (non-const version)
  /// @return Reference to this Offset with bitwise NOT applied
  Offset &operator~() {
    if (offset != UNKNOWN) {
      offset = ~offset;
    }
    return *this;
  }

  /// @brief Bitwise NOT operator (const version)
  /// @return A new Offset with bitwise NOT applied, or UNKNOWN
  Offset operator~() const {
    if (offset != UNKNOWN) {
      return Offset(~offset);
    }
    return Offset::UNKNOWN;
  }

  // strict comparision (no 'maybe' comparions
  // that arises due to UNKNOWN)
  /// @brief Less than comparison
  bool operator<(const Offset &o) const { return offset < o.offset; }

  /// @brief Greater than comparison
  bool operator>(const Offset &o) const { return offset > o.offset; }

  /// @brief Less than or equal comparison
  bool operator<=(const Offset &o) const { return offset <= o.offset; }

  /// @brief Greater than or equal comparison
  bool operator>=(const Offset &o) const { return offset >= o.offset; }

  /// @brief Equality comparison
  bool operator==(const Offset &o) const { return offset == o.offset; }

  /// @brief Inequality comparison
  bool operator!=(const Offset &o) const { return offset != o.offset; }

  /// @brief Check if offset is within a range
  /// @param from The lower bound (inclusive)
  /// @param to The upper bound (inclusive)
  /// @return true if offset is in [from, to]
  bool inRange(type from, type to) const {
    return (offset >= from && offset <= to);
  }

  /// @brief Check if offset is unknown
  /// @return true if offset equals UNKNOWN
  bool isUnknown() const { return offset == UNKNOWN; }
  /// @brief Check if offset is zero
  /// @return true if offset equals 0
  bool isZero() const { return offset == 0; }

  /// @brief Dereference operator to get the underlying value
  type operator*() const { return offset; }
  /// @brief Arrow operator to access the underlying value pointer
  const type *operator->() const { return &offset; }

#ifndef NDEBUG
  /// @brief Stream output operator for debugging
  friend std::ostream &operator<<(std::ostream &os, const Offset &o) {
    if (o.isUnknown())
      os << "?";
    else
      os << o.offset;
    return os;
  }

  /// @brief Print the offset to stdout
  void dump() const { std::cout << *this << "\n"; }
#endif // not NDEBUG

  /// @brief The underlying offset value
  type offset;
};

#include <functional>

namespace std {
/// @brief Hash specialization for Offset
template <> struct hash<Offset> {
  /// @brief Hash function operator
  /// @param o The Offset to hash
  /// @return The hash value
  size_t operator()(const Offset &o) const { return *o; }
};
} // namespace std

const Offset::type Offset::UNKNOWN = ~(static_cast<Offset::type>(0));

#endif
