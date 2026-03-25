#pragma once

#include "Utils/ADT/Iterator/IteratorFacade.h"

namespace util {

// CRTP class for adapting an iterator to a different type
template <typename Subclass, typename Iterator,
          typename CategoryT =
              typename std::iterator_traits<Iterator>::iterator_category,
          typename ValueT = typename std::iterator_traits<Iterator>::value_type,
          typename DifferenceT =
              typename std::iterator_traits<Iterator>::difference_type,
          typename PointerT = ValueT *, typename ReferenceT = ValueT &>
class IteratorAdaptor
    : public IteratorFacade<Subclass, CategoryT, ValueT, DifferenceT, PointerT,
                            ReferenceT> {
private:
  using IteratorTraits = std::iterator_traits<Iterator>;
  using BaseT = IteratorFacade<Subclass, CategoryT, ValueT, DifferenceT,
                               PointerT, ReferenceT>;

protected:
  Iterator itr;

  IteratorAdaptor() = default;
  IteratorAdaptor(const Iterator &i) : itr(i) {}
  IteratorAdaptor(Iterator &&i) : itr(std::move(i)) {}

public:
  Subclass &operator+=(DifferenceT n) {
    static_assert(
        BaseT::IsRandomAccess,
        "The '+=' operator is only defined for random access iterators.");
    itr += n;
    return *static_cast<Subclass *>(this);
  }

  Subclass &operator-=(DifferenceT n) {
    static_assert(
        BaseT::IsRandomAccess,
        "The '-=' operator is only defined for random access iterators.");
    itr -= n;
    return *static_cast<Subclass *>(this);
  }

  using BaseT::operator-;
  DifferenceT operator-(const Subclass &rhs) const {
    static_assert(
        BaseT::IsRandomAccess,
        "The '-' operator is only defined for random access iterators.");
    return itr - rhs.itr;
  }

  // We have to explicitly provide ++ and -- rather than letting the facade
  // forward to += because Iterator might not support +=.
  using BaseT::operator++;
  Subclass &operator++() {
    ++itr;
    return *static_cast<Subclass *>(this);
  }

  using BaseT::operator--;
  Subclass &operator--() {
    static_assert(
        BaseT::IsBidirectional,
        "The decrement operator is only defined for bidirectional iterators.");
    --itr;
    return *static_cast<Subclass *>(this);
  }

  bool operator==(const Subclass &rhs) const { return itr == rhs.itr; }
  bool operator<(const Subclass &rhs) const {
    static_assert(
        BaseT::IsRandomAccess,
        "Relational operators are only defined for random access iterators.");
    return itr < rhs.itr;
  }

  ReferenceT operator*() const { return *itr; }
};

} // namespace util
