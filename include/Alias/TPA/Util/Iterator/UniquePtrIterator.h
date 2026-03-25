#pragma once

#include "Alias/TPA/Util/Iterator/IteratorAdaptor.h"

namespace util {

// An iterator adapter that takes an iterator over a container of unique_ptrs
// and returns the corresponding iterator that automatically invoke the get()
// method of the unique_ptr Such an adapter is useful when we want to hide the
// ownership detail of a container when exposing its iterators
template <typename Iterator,
          typename ValueT =
              typename std::iterator_traits<Iterator>::value_type::pointer>
class UniquePtrIterator
    : public IteratorAdaptor<
          UniquePtrIterator<Iterator>, Iterator,
          typename std::iterator_traits<Iterator>::iterator_category, ValueT> {
private:
  using BaseT = IteratorAdaptor<
      UniquePtrIterator<Iterator>, Iterator,
      typename std::iterator_traits<Iterator>::iterator_category, ValueT>;

public:
  UniquePtrIterator() = default;
  UniquePtrIterator(const Iterator &i) : BaseT(i) {}
  UniquePtrIterator(Iterator &&i) : BaseT(std::move(i)) {}

  ValueT operator*() const { return this->itr->get(); }
};

template <typename Iterator,
          typename ValueT =
              typename std::iterator_traits<Iterator>::value_type::pointer>
class UniquePtrConstIterator
    : public IteratorAdaptor<
          UniquePtrConstIterator<Iterator>, Iterator,
          typename std::iterator_traits<Iterator>::iterator_category, ValueT> {
private:
  using BaseT = IteratorAdaptor<
      UniquePtrConstIterator<Iterator>, Iterator,
      typename std::iterator_traits<Iterator>::iterator_category, ValueT>;

public:
  UniquePtrConstIterator() = default;
  UniquePtrConstIterator(const Iterator &i) : BaseT(i) {}
  UniquePtrConstIterator(Iterator &&i) : BaseT(std::move(i)) {}

  ValueT operator*() const { return this->itr->get(); }
};

} // namespace util