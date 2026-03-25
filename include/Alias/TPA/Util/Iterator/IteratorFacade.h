#pragma once

#include <iterator>
#include <type_traits>

namespace util {

// CRTP class that implements a simple iterator facade utility
template <typename Subclass, typename CategoryT, typename ValueT,
          typename DifferenceT = std::ptrdiff_t, typename PointerT = ValueT *,
          typename ReferenceT = ValueT &>
class IteratorFacade {
protected:
  static constexpr bool IsRandomAccess =
      std::is_base_of<std::random_access_iterator_tag, CategoryT>::value;
  static constexpr bool IsBidirectional =
      std::is_base_of<std::bidirectional_iterator_tag, CategoryT>::value;

public:
  // Provide standard iterator typedefs without inheriting from std::iterator
  // (deprecated since C++17).
  using iterator_category = CategoryT;
  using value_type = ValueT;
  using difference_type = DifferenceT;
  using pointer = PointerT;
  using reference = ReferenceT;

  Subclass operator+(DifferenceT n) const {
    static_assert(
        IsRandomAccess,
        "The '+' operator is only defined for random access iterators.");
    Subclass tmp = *static_cast<const Subclass *>(this);
    tmp += n;
    return tmp;
  }

  friend Subclass operator+(DifferenceT n, const Subclass &i) {
    static_assert(
        IsRandomAccess,
        "The '+' operator is only defined for random access iterators.");
    return i + n;
  }

  Subclass operator-(DifferenceT n) const {
    static_assert(
        IsRandomAccess,
        "The '-' operator is only defined for random access iterators.");
    Subclass tmp = *static_cast<const Subclass *>(this);
    tmp -= n;
    return tmp;
  }

  Subclass &operator++() {
    return static_cast<Subclass *>(this)->operator+=(1);
  }

  Subclass operator++(int) {
    Subclass tmp = *static_cast<Subclass *>(this);
    ++*static_cast<Subclass *>(this);
    return tmp;
  }

  Subclass &operator--() {
    static_assert(
        IsBidirectional,
        "The decrement operator is only defined for bidirectional iterators.");
    return static_cast<Subclass *>(this)->operator-=(1);
  }

  Subclass operator--(int) {
    static_assert(
        IsBidirectional,
        "The decrement operator is only defined for bidirectional iterators.");
    Subclass tmp = *static_cast<Subclass *>(this);
    --*static_cast<Subclass *>(this);
    return tmp;
  }

  bool operator!=(const Subclass &rhs) const {
    return !static_cast<const Subclass *>(this)->operator==(rhs);
  }

  bool operator>(const Subclass &rhs) const {
    static_assert(
        IsRandomAccess,
        "Relational operators are only defined for random access iterators.");
    return !static_cast<const Subclass *>(this)->operator<(rhs) &&
           !static_cast<const Subclass *>(this)->operator==(rhs);
  }

  bool operator<=(const Subclass &rhs) const {
    static_assert(
        IsRandomAccess,
        "Relational operators are only defined for random access iterators.");
    return !static_cast<const Subclass *>(this)->operator>(rhs);
  }

  bool operator>=(const Subclass &rhs) const {
    static_assert(
        IsRandomAccess,
        "Relational operators are only defined for random access iterators.");
    return !static_cast<const Subclass *>(this)->operator<(rhs);
  }

  PointerT operator->() const {
    return &static_cast<const Subclass *>(this)->operator*();
  }

  ReferenceT operator[](DifferenceT n) const {
    static_assert(IsRandomAccess,
                  "Subscripting is only defined for random access iterators.");
    return *static_cast<const Subclass *>(this)->operator+(n);
  }
};

} // namespace util
